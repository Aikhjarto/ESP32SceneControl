// ESP32PWM — servo scene controller, application entry point.

#include "app/App.h"
#include "app/Commands.h"
#include "app/MotionTask.h"
#include "app/Persist.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "net/CaptiveDns.h"
#include "net/WebServer.h"
#include "net/Wifi.h"
#include "nvs_flash.h"

static const char* TAG = "main";

struct LogSink : ss::ReplySink {
    void line(const std::string& text) override { ESP_LOGW(TAG, "%s", text.c_str()); }
};

static void initNvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition invalid, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void logChipInfo() {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flashSize = 0;
    esp_flash_get_size(nullptr, &flashSize);
    ESP_LOGI(TAG, "ESP32PWM, ESP-IDF %s, %d core(s), rev %d.%d, %lu MB flash", esp_get_idf_version(), chip.cores,
             chip.revision / 100, chip.revision % 100, static_cast<unsigned long>(flashSize >> 20));
}

extern "C" void app_main() {
    using namespace ss;
    logChipInfo();
    initNvs();
    app::initLock();

    // Defaults, then the saved settings replayed through the same parser as serial/WebSocket.
    setDefaults(app::config());
    std::string saved;
    if (app::persist::load(saved)) {
        LogSink log;
        app::parser().replay(saved, log);
        ESP_LOGI(TAG, "config loaded (%u bytes)", static_cast<unsigned>(saved.size()));
    }

    app::outputs().begin(app::config(), app::bootLog());
    app::startMotion();
    app::startCommands();

    const NetConfig net = app::config().net;
    net::startAccessPoint(net);
    if (net.captive) net::startCaptiveDns(net::apIp());
    net::startWebServer(net::apIp(), net.captive);
}
