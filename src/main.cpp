// ESP32PWM — servo scene controller, application entry point.

#include <string>

#include "core/Config.h"
#include "core/Motion.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "out/Outputs.h"

static const char* TAG = "main";

static ss::Config gConfig;
static ss::Outputs gOutputs;

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
    logChipInfo();
    initNvs();

    ss::setDefaults(gConfig);
    std::string problems;
    gOutputs.begin(gConfig, problems);

    // Hold every servo at its start angle (output test until the motion engine exists).
    for (int i = 0; i < ss::kMaxServos; ++i) {
        const ss::ServoConfig& s = gConfig.servo[i];
        if (!ss::servoActive(s) || gOutputs.channel(i).kind == ss::HwKind::None) continue;
        gOutputs.setPulseUs(i, ss::pulseUs(s, s.startDeg));
        ESP_LOGI(TAG, "servo %d -> %s", i, gOutputs.describe(i).c_str());
    }
    gOutputs.flush();
}
