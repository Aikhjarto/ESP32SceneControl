#include "app/Commands.h"

#include <cstring>
#include <strings.h>

#include "app/App.h"
#include "app/MotionTask.h"
#include "app/Persist.h"
#include "core/LineReader.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net/Ota.h"
#include "net/WebServer.h"
#include "net/Wifi.h"
#include "soc/soc_caps.h"

namespace ss::app {

static const char* TAG = "cmd";

constexpr uart_port_t kUartPort = UART_NUM_0;

static WsSend gWsSend;
static WsBroadcast gWsBroadcast;
static QueueHandle_t gQueue;

struct Request {
    ChannelId ch;
    std::string line;
};

// ---------------------------------------------------------------------------
// Output routing

static void sendTo(ChannelId ch, const std::string& text) {
    if (text.empty()) return;
    if (ch == kUart) {
        uart_write_bytes(kUartPort, text.data(), text.size());
    } else if (gWsSend) {
        gWsSend(ch - 1, text);
    }
}

static void broadcastAll(const std::string& text) {
    std::string withNl = text + "\n";
    uart_write_bytes(kUartPort, withNl.data(), withNl.size());
    if (gWsBroadcast) gWsBroadcast(withNl);
}

// Collects a whole reply so it goes out as one UART write / one WebSocket frame.
struct BufferSink : ReplySink {
    std::string text;
    void line(const std::string& t) override {
        text += t;
        text += '\n';
    }
};

// ---------------------------------------------------------------------------
// Host services for the parser

class AppHost : public Host {
public:
    void lock() override { app::lock(); }
    void unlock() override { app::unlock(); }
    void setRunning(bool run, const char* source) override { app::setRunning(run, source); }
    bool save(const std::string& text, std::string& err) override { return persist::save(text, err); }
    bool erase(std::string& err) override { return persist::erase(err); }

    void reboot() override { restartSoon(); }

    Err command(const std::string& name, const std::string& value, bool hasValue, ReplySink& out,
                std::string& msg) override {
        if (strcasecmp(name.c_str(), "Firmware/Update") != 0) return Err::UnknownCommand;
        std::string url = hasValue && !value.empty() ? value : net::defaultUpdateUrl();
        if (url.empty()) {
            msg = "no client has joined the access point yet; use $Firmware/Update=http://<ip>:9999/firmware.bin";
            return Err::BadValue;
        }
        if (!net::startOtaDownload(url, msg)) return Err::BadValue;
        out.line("[OTA:start " + url + "]");
        return Err::Ok;
    }

    void info(ReplySink& out) override {
        char buf[160];
        const esp_app_desc_t* app = esp_app_get_description();
        snprintf(buf, sizeof(buf), "[VER:%s ESP32PWM]", app->version);
        out.line(buf);
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        snprintf(buf, sizeof(buf), "[OPT:ESP32 rev %d.%d, ESP-IDF %s]", chip.revision / 100, chip.revision % 100,
                 esp_get_idf_version());
        out.line(buf);
        out.line(net::firmwareInfo());
        if (!net::defaultUpdateUrl().empty()) out.line("[UPDATE:" + net::defaultUpdateUrl() + "]");
        if (!net::apSsid().empty()) {
            snprintf(buf, sizeof(buf), "[MSG:INFO: AP \"%s\" http://%s/ (%d WebSocket clients)]",
                     net::apSsid().c_str(), net::apIp().c_str(), net::wsClientCount());
            out.line(buf);
        }

        Locked l;
        const Config& cfg = config();
        const Outputs& o = outputs();
        snprintf(buf, sizeof(buf), "[MSG:INFO: PWM in use: LEDC-HS %d/%d, LEDC-LS %d/%d, MCPWM0 %d/%d, MCPWM1 %d/%d]",
                 o.used(HwKind::LedcHs), SOC_LEDC_CHANNEL_NUM, o.used(HwKind::LedcLs), SOC_LEDC_CHANNEL_NUM,
                 o.used(HwKind::Mcpwm0), Outputs::kMcpwmPerUnit, o.used(HwKind::Mcpwm1), Outputs::kMcpwmPerUnit);
        out.line(buf);
        for (int b = 0; b < kMaxPca; ++b) {
            if (!cfg.pca[b].enable) continue;
            snprintf(buf, sizeof(buf), "[MSG:INFO: PCA9685 board %d at 0x%02lX %s]", b,
                     static_cast<unsigned long>(cfg.pca[b].addr), o.pcaFound(b) ? "found" : "NOT found");
            out.line(buf);
        }
        for (int i = 0; i < kMaxServos; ++i) {
            const ServoConfig& s = cfg.servo[i];
            if (s.pin.type == PinType::None) continue;
            snprintf(buf, sizeof(buf), "[SERVO:%d %s %s%s]", i, Settings::formatPin(s.pin).c_str(),
                     o.describe(i).c_str(), s.enable ? "" : " disabled");
            out.line(buf);
        }
        const std::string& log = bootLog();
        size_t pos = 0;
        while (pos < log.size()) {
            size_t nl = log.find('\n', pos);
            if (nl == std::string::npos) nl = log.size();
            out.line("[MSG:WARN: " + log.substr(pos, nl - pos) + "]");
            pos = nl + 1;
        }
    }
};

Parser& parser() {
    static AppHost host;
    static Parser p(config(), scene(), host);
    return p;
}

// ---------------------------------------------------------------------------
// Executor and serial channel

void submitLine(ChannelId ch, const std::string& line) {
    auto* req = new Request{ch, line};
    if (xQueueSend(gQueue, &req, 0) != pdTRUE) {
        delete req;
        sendTo(ch, "[MSG:ERR: Busy, command dropped]\nerror:3\n");
    }
}

void submitRealtime(ChannelId ch, char c) {
    BufferSink sink;
    parser().realtime(c, sink);
    sendTo(ch, sink.text);
}

static void executorTask(void*) {
    for (;;) {
        Request* req;
        xQueueReceive(gQueue, &req, portMAX_DELAY);
        BufferSink sink;
        parser().execute(req->line, sink);
        sendTo(req->ch, sink.text);
        delete req;
    }
}

static void uartTask(void*) {
    LineReader reader;
    uint8_t buf[64];
    for (;;) {
        int n = uart_read_bytes(kUartPort, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; ++i) {
            switch (reader.feed(static_cast<char>(buf[i]))) {
                case LineReader::Event::Realtime:
                    submitRealtime(kUart, reader.realtimeChar());
                    break;
                case LineReader::Event::Line:
                    submitLine(kUart, reader.line());
                    break;
                default:
                    break;
            }
        }
    }
}

void setWsTransport(WsSend send, WsBroadcast broadcast) {
    gWsSend = send;
    gWsBroadcast = broadcast;
}

void startCommands() {
    // Writers block while a large reply drains, so time-critical tasks must not write here
    // (they use app::broadcast, which hands the text to a background task).
    ESP_ERROR_CHECK(uart_driver_install(kUartPort, 1024, 4096, 0, nullptr, 0));
    // Route stdout/ESP_LOG through the driver too, so log lines can't land inside replies.
    uart_vfs_dev_use_driver(kUartPort);
    gQueue = xQueueCreate(16, sizeof(Request*));
    setBroadcaster(broadcastAll);
    xTaskCreatePinnedToCore(executorTask, "cmd", 6144, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(uartTask, "uart", 3072, nullptr, 5, nullptr, 0);
    ESP_LOGI(TAG, "serial command channel ready (try $H)");
}

}  // namespace ss::app
