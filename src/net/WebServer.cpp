#include "net/WebServer.h"

#include <cstring>

#include "app/App.h"
#include "app/Commands.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/Ota.h"

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[] asm("_binary_app_js_gz_end");
extern const uint8_t style_css_gz_start[] asm("_binary_style_css_gz_start");
extern const uint8_t style_css_gz_end[] asm("_binary_style_css_gz_end");

namespace ss::net {

static const char* TAG = "web";

constexpr size_t kMaxWsFrame = 4096;
constexpr size_t kMaxConfigUpload = 32768;
constexpr int kStatusPeriodMs = 200;

static httpd_handle_t gServer;
static std::string gRedirect;

int wsClientCount() {
    if (!gServer) return 0;
    size_t n = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(gServer, &n, fds) != ESP_OK) return 0;
    int count = 0;
    for (size_t i = 0; i < n; ++i) count += httpd_ws_get_fd_info(gServer, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET;
    return count;
}

// ---------------------------------------------------------------------------
// Static files

struct Asset {
    const char* uri;
    const char* type;
    const uint8_t* start;
    const uint8_t* end;
};

static const Asset kAssets[] = {
    {"/", "text/html", index_html_gz_start, index_html_gz_end},
    {"/app.js", "application/javascript", app_js_gz_start, app_js_gz_end},
    {"/style.css", "text/css", style_css_gz_start, style_css_gz_end},
};

static esp_err_t serveAsset(httpd_req_t* req) {
    const Asset* a = static_cast<const Asset*>(req->user_ctx);
    httpd_resp_set_type(req, a->type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, reinterpret_cast<const char*>(a->start), a->end - a->start);
}

// ---------------------------------------------------------------------------
// WebSocket

struct WsMessage {
    int fd;
    std::string text;
};

static void wsSendWork(void* arg) {
    auto* m = static_cast<WsMessage*>(arg);
    httpd_ws_frame_t frame = {};
    frame.final = true;
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(m->text.data());
    frame.len = m->text.size();
    httpd_ws_send_frame_async(gServer, m->fd, &frame);
    delete m;
}

static void wsSend(int fd, const std::string& text) {
    if (!gServer || httpd_ws_get_fd_info(gServer, fd) != HTTPD_WS_CLIENT_WEBSOCKET) return;
    auto* m = new WsMessage{fd, text};
    if (httpd_queue_work(gServer, wsSendWork, m) != ESP_OK) delete m;
}

static void wsBroadcast(const std::string& text) {
    if (!gServer) return;
    size_t n = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(gServer, &n, fds) != ESP_OK) return;
    for (size_t i = 0; i < n; ++i) wsSend(fds[i], text);
}

static esp_err_t wsHandler(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {  // handshake done
        ESP_LOGI(TAG, "WebSocket client %d connected", fd);
        return ESP_OK;
    }
    httpd_ws_frame_t frame = {};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;
    if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0) return ESP_OK;
    if (frame.len > kMaxWsFrame) return ESP_FAIL;

    std::string text(frame.len, '\0');
    frame.payload = reinterpret_cast<uint8_t*>(text.data());
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) return err;

    const app::ChannelId ch = app::wsChannel(fd);
    if (text.size() == 1 && Parser::isRealtime(text[0])) {
        app::submitRealtime(ch, text[0]);
        return ESP_OK;
    }
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) app::submitLine(ch, line);
        pos = nl + 1;
    }
    return ESP_OK;
}

// Pushes status and positions to every WebSocket client a few times per second.
static void statusTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kStatusPeriodMs));
        if (wsClientCount() == 0) continue;
        std::string text;
        {
            app::Locked l;
            text = app::scene().status() + "\n" + app::scene().positions() + "\n";
        }
        wsBroadcast(text);
    }
}

// ---------------------------------------------------------------------------
// Config backup / restore

static esp_err_t configGet(httpd_req_t* req) {
    std::string text;
    {
        app::Locked l;
        text = app::parser().settings().dumpChanged();
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"servoscene-config.txt\"");
    return httpd_resp_send(req, text.data(), text.size());
}

static esp_err_t configPost(httpd_req_t* req) {
    if (req->content_len > kMaxConfigUpload) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "config too large");
    }
    std::string body(req->content_len, '\0');
    size_t got = 0;
    while (got < body.size()) {
        int r = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    struct Sink : ReplySink {
        std::string text;
        void line(const std::string& t) override { text += t + "\n"; }
    } sink;
    {
        app::Locked l;  // one atomic restore, not interleaved with other channels
        app::parser().replay(body, sink);
    }
    sink.text += "ok\n";
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, sink.text.data(), sink.text.size());
}

// Phones probe URLs like /generate_204 or /hotspot-detect.html; redirecting them to the UI
// makes the OS show the captive portal page.
static esp_err_t redirect(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", gRedirect.c_str());
    return httpd_resp_send(req, nullptr, 0);
}

void startWebServer(const std::string& ip, bool captive) {
    gRedirect = "http://" + ip + "/";

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 8192;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&gServer, &cfg));

    for (const Asset& a : kAssets) {
        httpd_uri_t u = {};
        u.uri = a.uri;
        u.method = HTTP_GET;
        u.handler = serveAsset;
        u.user_ctx = const_cast<Asset*>(&a);
        httpd_register_uri_handler(gServer, &u);
    }

    httpd_uri_t ws = {};
    ws.uri = "/ws";
    ws.method = HTTP_GET;
    ws.handler = wsHandler;
    ws.is_websocket = true;
    httpd_register_uri_handler(gServer, &ws);

    httpd_uri_t cget = {};
    cget.uri = "/config.txt";
    cget.method = HTTP_GET;
    cget.handler = configGet;
    httpd_register_uri_handler(gServer, &cget);

    httpd_uri_t cpost = cget;
    cpost.method = HTTP_POST;
    cpost.handler = configPost;
    httpd_register_uri_handler(gServer, &cpost);

    httpd_uri_t update = {};
    update.uri = "/update";
    update.method = HTTP_POST;
    update.handler = otaUploadHandler;
    httpd_register_uri_handler(gServer, &update);

    if (captive) {
        httpd_uri_t any = {};
        any.uri = "/*";
        any.method = HTTP_GET;
        any.handler = redirect;
        httpd_register_uri_handler(gServer, &any);
    }

    app::setWsTransport(wsSend, wsBroadcast);
    xTaskCreatePinnedToCore(statusTask, "status", 4096, nullptr, 3, nullptr, 0);
    ESP_LOGI(TAG, "web server on http://%s/", ip.c_str());
}

}  // namespace ss::net
