#include "app/Commands.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
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
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "net/Ota.h"
#include "net/WebServer.h"
#include "net/Wifi.h"
#include "soc/soc_caps.h"

namespace ss::app {

static const char* TAG = "cmd";

constexpr uart_port_t kUartPort = UART_NUM_0;

constexpr size_t kUartRingBytes = 8192;
constexpr size_t kUartChunk = 4000;  // reply chunk size (ring item limit 4088); ends at a line break
constexpr TickType_t kSubmitWait = pdMS_TO_TICKS(100);

static WsSend gWsSend;
static WsBroadcast gWsBroadcast;
static QueueHandle_t gQueue;
static RingbufHandle_t gUartOut;
static std::atomic<uint32_t> gLogDropped;

struct Request {
    ChannelId ch;
    std::string line;
};

uint32_t droppedLogLines() { return gLogDropped; }

// ---------------------------------------------------------------------------
// Serial output: everything (replies, broadcasts, log lines) goes through one ring buffer
// drained by one writer task. Items are whole lines, so output never interleaves inside a
// line, and a caller only waits as long as it chooses to.

static void uartWriterTask(void*) {
    for (;;) {
        size_t len = 0;
        void* item = xRingbufferReceive(gUartOut, &len, portMAX_DELAY);
        uart_write_bytes(kUartPort, item, len);
        vRingbufferReturnItem(gUartOut, item);
    }
}

static bool uartOut(const char* data, size_t len, TickType_t wait) {
    while (len > 0) {
        size_t n = std::min(len, kUartChunk);
        if (n < len) {  // end the chunk after its last complete line
            size_t cut = n;
            while (cut > 0 && data[cut - 1] != '\n') --cut;
            if (cut > 0) n = cut;
        }
        if (xRingbufferSend(gUartOut, data, n, wait) != pdTRUE) return false;
        data += n;
        len -= n;
    }
    return true;
}

// ESP_LOG hook: one call per log line. Never waits, so logging from the motion task or a
// timer callback cannot stall them; if the buffer is full the line is dropped and counted.
static int logToUart(const char* fmt, va_list ap) {
    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n <= 0) return n;
    size_t len = static_cast<size_t>(n);
    if (len >= sizeof(line)) {
        len = sizeof(line) - 1;
        line[len - 1] = '\n';
    }
    if (xRingbufferSend(gUartOut, line, len, 0) != pdTRUE) ++gLogDropped;
    return n;
}

static void sendTo(ChannelId ch, std::string text, TickType_t wait = portMAX_DELAY) {
    if (text.empty()) return;
    if (ch == kUart) {
        uartOut(text.data(), text.size(), wait);
    } else if (gWsSend) {
        gWsSend(ch, std::move(text));  // replies can be tens of KB: don't copy
    }
}

static void broadcastAll(const std::string& text) {
    std::string withNl = text + "\n";
    uartOut(withNl.data(), withNl.size(), portMAX_DELAY);
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
        // Minimum free stack per task since boot (bytes), to check headroom on real hardware.
        std::string stacks;
        for (const char* name : {"motion", "cmd", "uart", "uart_out", "broadcast", "status", "httpd", "dns", "ota",
                                 "sys_evt", "tcpip_thread", "esp_timer"}) {
            if (TaskHandle_t t = xTaskGetHandle(name)) {
                stacks += std::string(stacks.empty() ? "" : ", ") + name + "=" +
                          std::to_string(uxTaskGetStackHighWaterMark(t));
            }
        }
        out.line("[MSG:INFO: Stack free (bytes): " + stacks + "]");
        snprintf(buf, sizeof(buf), "[MSG:INFO: Heap free %u bytes, largest block %u, lowest since boot %u]",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)));
        out.line(buf);
        if (droppedLogLines() > 0) {
            out.line("[MSG:INFO: " + std::to_string(droppedLogLines()) + " log lines dropped (serial output was busy)]");
        }
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
        for (int b = 0; b < kMaxPcf; ++b) {
            if (!cfg.pcf[b].enable) continue;
            snprintf(buf, sizeof(buf), "[MSG:INFO: %s board %d at 0x%02lX %s]",
                     cfg.pcf[b].type == PcfType::Pcf8575 ? "PCF8575" : "PCF8574", b,
                     static_cast<unsigned long>(cfg.pcf[b].addr), o.pcfFound(b) ? "found" : "NOT found");
            out.line(buf);
        }
        for (int i = 0; i < kMaxServos; ++i) {
            const ServoConfig& s = cfg.servo[i];
            if (s.pin.type == PinType::None) continue;
            snprintf(buf, sizeof(buf), "[SERVO:%d %s %s%s]", i, Settings::formatPin(s.pin).c_str(),
                     o.describe(i).c_str(), s.enable ? "" : " disabled");
            out.line(buf);
        }
        for (int i = 0; i < kMaxOutputs; ++i) {
            const OutputConfig& d = cfg.out[i];
            if (d.pin.type == PinType::None) continue;
            snprintf(buf, sizeof(buf), "[OUTPUT:%d %s %s%s]", i, Settings::formatPin(d.pin).c_str(),
                     o.describeOutput(i).c_str(), d.enable ? "" : " disabled");
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

bool submitLine(ChannelId ch, const std::string& line) {
    auto* req = new Request{ch, line};
    if (xQueueSend(gQueue, &req, kSubmitWait) == pdTRUE) return true;
    delete req;
    // Serial is read by people: say so. WebSocket callers resync the client instead.
    if (ch == kUart) sendTo(ch, "[MSG:ERR: Busy, command dropped]\nerror:3\n", kSubmitWait);
    return false;
}

void submitRealtime(ChannelId ch, char c) {
    BufferSink sink;
    parser().realtime(c, sink);  // acts first; the reply must not hold up the next realtime char
    sendTo(ch, std::move(sink.text), ch == kUart ? pdMS_TO_TICKS(50) : portMAX_DELAY);
}

static void executorTask(void*) {
    for (;;) {
        Request* req;
        xQueueReceive(gQueue, &req, portMAX_DELAY);
        BufferSink sink;
        parser().execute(req->line, sink);
        sendTo(req->ch, std::move(sink.text));
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
                case LineReader::Event::TooLong:
                    sendTo(kUart, "[MSG:ERR: Line too long (max 256 characters), ignored]\nerror:2\n", pdMS_TO_TICKS(50));
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
    ESP_ERROR_CHECK(uart_driver_install(kUartPort, 1024, 1024, 0, nullptr, 0));
    uart_vfs_dev_use_driver(kUartPort);  // any stray printf also goes through the driver
    gUartOut = xRingbufferCreate(kUartRingBytes, RINGBUF_TYPE_NOSPLIT);
    xTaskCreatePinnedToCore(uartWriterTask, "uart_out", 3072, nullptr, 3, nullptr, 0);
    esp_log_set_vprintf(logToUart);
    gQueue = xQueueCreate(16, sizeof(Request*));
    setBroadcaster(broadcastAll);
    xTaskCreatePinnedToCore(executorTask, "cmd", 6144, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(uartTask, "uart", 4096, nullptr, 5, nullptr, 0);
    ESP_LOGI(TAG, "serial command channel ready (try $H)");
}

}  // namespace ss::app
