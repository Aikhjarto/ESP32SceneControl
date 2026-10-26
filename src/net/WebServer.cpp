#include "net/WebServer.h"

#include <atomic>
#include <cstring>

#include "app/App.h"
#include "app/Commands.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "net/HttpRecv.h"
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

// All WebSocket sends run in the single httpd task (httpd_queue_work). Its control queue
// is small, so status pushes are dropped when it is busy, but command replies are not:
// the UI pairs replies with requests by order, and a lost reply would mismatch every
// later one.
struct WsMessage {
    int fd;
    uint32_t generation;  // connection the message belongs to (see gGeneration)
    std::string text;
    bool status;
};

static TaskHandle_t gHttpdTask;           // learned on the first WebSocket request
static std::atomic<int> gStatusInFlight;  // queued status pushes not yet sent
// Connection generation per socket, bumped on every WebSocket handshake. lwIP reuses socket
// numbers, so a message for a closed connection must not go to the next client on that fd.
static std::atomic<uint32_t> gGeneration[256];
// Connection whose reply had to be dropped: it is closed instead of being answered out of order.
static std::atomic<uint32_t> gStale[256];
static_assert(LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS <= 256, "socket numbers must fit gGeneration");
static uint32_t gNextGeneration;  // httpd task only

// Sends everything or fails. A socket send may be cut short by the send timeout on a slow
// link (httpd_ws_send_frame_async treats that as success and corrupts the stream).
// Gives up after two consecutive send timeouts (2 s each), so one dead client blocks the
// single server task for at most ~4 s.
static bool sendAll(int fd, const char* p, size_t n) {
    int stalls = 0;
    while (n > 0) {
        int r = httpd_socket_send(gServer, fd, p, n, 0);
        if (r > 0) {
            p += r;
            n -= r;
            stalls = 0;
        } else if (r != HTTPD_SOCK_ERR_TIMEOUT || ++stalls >= 2) {
            return false;
        }
    }
    return true;
}

static bool sendTextFrame(int fd, const std::string& text) {
    uint8_t hdr[10];
    size_t h = 0;
    const uint64_t len = text.size();
    hdr[h++] = 0x81;  // FIN, text frame (server frames are not masked)
    if (len < 126) {
        hdr[h++] = static_cast<uint8_t>(len);
    } else if (len < 65536) {
        hdr[h++] = 126;
        hdr[h++] = static_cast<uint8_t>(len >> 8);
        hdr[h++] = static_cast<uint8_t>(len);
    } else {
        hdr[h++] = 127;
        for (int i = 7; i >= 0; --i) hdr[h++] = static_cast<uint8_t>(len >> (8 * i));
    }
    return sendAll(fd, reinterpret_cast<const char*>(hdr), h) && sendAll(fd, text.data(), text.size());
}

static void wsSendWork(void* arg) {
    auto* m = static_cast<WsMessage*>(arg);
    const int slot = m->fd & 0xFF;
    if (gGeneration[slot] == m->generation && httpd_ws_get_fd_info(gServer, m->fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        if (gStale[slot] == m->generation) {
            httpd_sess_trigger_close(gServer, m->fd);  // never send after a dropped reply
        } else if (!sendTextFrame(m->fd, m->text)) {
            // Dead or stuck client: drop it instead of blocking every later send on it.
            ESP_LOGW(TAG, "WebSocket client %d not responding, closing", m->fd);
            httpd_sess_trigger_close(gServer, m->fd);
        }
    }
    if (m->status) --gStatusInFlight;
    delete m;
}

static void wsQueue(int fd, uint32_t generation, std::string text, bool status) {
    if (!gServer || httpd_ws_get_fd_info(gServer, fd) != HTTPD_WS_CLIENT_WEBSOCKET) return;
    auto* m = new WsMessage{fd, generation, std::move(text), status};
    if (status) ++gStatusInFlight;
    if (xTaskGetCurrentTaskHandle() == gHttpdTask) {  // already in the httpd task (realtime replies)
        wsSendWork(m);
        return;
    }
    for (int attempt = 0; httpd_queue_work(gServer, wsSendWork, m) != ESP_OK; ++attempt) {
        if (status || attempt >= 50) {  // replies: retry for up to ~0.5 s
            if (status) {
                --gStatusInFlight;
            } else {
                gStale[fd & 0xFF] = generation;  // the client would pair later replies wrongly
            }
            ESP_LOGW(TAG, "WebSocket %s to client %d dropped", status ? "status" : "reply", fd);
            delete m;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void wsSend(app::ChannelId ch, std::string text) {
    wsQueue(app::wsFd(ch), app::wsGeneration(ch), std::move(text), false);
}

// Runs in the server task for every accepted socket. In ESP-IDF 6 the WebSocket handshake
// is answered internally and never reaches wsHandler, so a new connection gets its
// generation here.
static esp_err_t onOpen(httpd_handle_t, int fd) {
    gHttpdTask = xTaskGetCurrentTaskHandle();
    gGeneration[fd & 0xFF] = ++gNextGeneration;
    return ESP_OK;
}

static void wsBroadcastAll(const std::string& text, bool status) {
    if (!gServer) return;
    size_t n = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(gServer, &n, fds) != ESP_OK) return;
    for (size_t i = 0; i < n; ++i) wsQueue(fds[i], gGeneration[fds[i] & 0xFF], text, status);
}

static void wsBroadcast(const std::string& text) { wsBroadcastAll(text, false); }

static esp_err_t wsHandler(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) return ESP_OK;  // handshake (not reached in ESP-IDF 6)
    if (gStale[fd & 0xFF] == gGeneration[fd & 0xFF]) return ESP_FAIL;  // a reply was lost: resync

    httpd_ws_frame_t frame = {};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;
    if (frame.len > kMaxWsFrame) return ESP_FAIL;
    // Always consume the payload (also of binary/pong frames), or its bytes would be parsed
    // as the next frame.
    std::string text(frame.len, '\0');
    if (frame.len > 0) {
        frame.payload = reinterpret_cast<uint8_t*>(text.data());
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err != ESP_OK) return err;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT || text.empty()) return ESP_OK;

    const app::ChannelId ch = app::wsChannel(fd, gGeneration[fd & 0xFF]);
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
        if (!line.empty() && !app::submitLine(ch, line)) {
            // Executor stays busy: a "busy" reply now would overtake pending replies and be
            // paired with the wrong request. Close instead; the UI reconnects and reloads.
            // Returning an error makes the server close the connection right away.
            ESP_LOGW(TAG, "command queue full, resyncing WebSocket client %d", fd);
            return ESP_FAIL;
        }
        pos = nl + 1;
    }
    return ESP_OK;
}

// Pushes status and positions to every WebSocket client a few times per second.
static void statusTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kStatusPeriodMs));
        // Skip a round while the previous one is still queued (busy or stuck server).
        if (wsClientCount() == 0 || gStatusInFlight > 0) continue;
        std::string text;
        {
            app::Locked l;
            text = app::scene().status() + "\n" + app::scene().positions() + "\n" + app::scene().outputs() + "\n";
        }
        wsBroadcastAll(text, true);
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
    if (!recvExact(req, body.data(), body.size())) return ESP_FAIL;
    struct Sink : ReplySink {
        std::string text;
        void line(const std::string& t) override { text += t + "\n"; }
    } sink;
    bool ok;
    {
        app::Locked l;  // one atomic restore, not interleaved with other channels
        ok = app::parser().restore(body, sink);
    }
    sink.text += ok ? "ok\n" : "error:2\n";
    httpd_resp_set_status(req, ok ? "200 OK" : "400 Bad Request");
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
    // Detect phones that left the access point without closing the connection (~20 s),
    // and don't let a stuck client block the server for long.
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 5;
    cfg.keep_alive_interval = 5;
    cfg.keep_alive_count = 3;
    cfg.send_wait_timeout = 2;
    cfg.open_fn = onOpen;
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
