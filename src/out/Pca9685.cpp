#include "out/Pca9685.h"

#include <algorithm>
#include <cmath>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ss {

static const char* TAG = "pca9685";

namespace reg {
constexpr uint8_t kMode1 = 0x00;
constexpr uint8_t kMode2 = 0x01;
constexpr uint8_t kLed0OnL = 0x06;
constexpr uint8_t kAllLedOffH = 0xFD;
constexpr uint8_t kPrescale = 0xFE;
}  // namespace reg

constexpr uint8_t kMode1Restart = 0x80;
constexpr uint8_t kMode1AutoInc = 0x20;
constexpr uint8_t kMode1Sleep = 0x10;
constexpr uint8_t kMode2OutDrv = 0x04;
constexpr uint8_t kFullOff = 0x10;  // bit 4 of LEDn_OFF_H
constexpr int kTimeoutMs = 20;

bool Pca9685::writeReg(uint8_t r, uint8_t value) {
    uint8_t buf[2] = {r, value};
    return i2c_master_transmit(dev_, buf, sizeof(buf), kTimeoutMs) == ESP_OK;
}

bool Pca9685::begin(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t sclHz, uint32_t freqHz, uint32_t oscHz) {
    if (i2c_master_probe(bus, addr, kTimeoutMs) != ESP_OK) return false;

    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = addr;
    dc.scl_speed_hz = sclHz;
    if (i2c_master_bus_add_device(bus, &dc, &dev_) != ESP_OK) {
        dev_ = nullptr;
        return false;
    }

    int prescale = static_cast<int>(std::lround(static_cast<double>(oscHz) / (4096.0 * freqHz))) - 1;
    prescale = std::clamp(prescale, 3, 255);
    freqHz_ = static_cast<float>(oscHz) / (4096.0f * (prescale + 1));

    bool ok = writeReg(reg::kMode1, kMode1Sleep)  // prescaler can only be set while sleeping
              && writeReg(reg::kPrescale, static_cast<uint8_t>(prescale)) && writeReg(reg::kMode2, kMode2OutDrv) &&
              writeReg(reg::kAllLedOffH, kFullOff) && writeReg(reg::kMode1, kMode1AutoInc);
    vTaskDelay(pdMS_TO_TICKS(2));  // oscillator start-up
    ok = ok && writeReg(reg::kMode1, kMode1Restart | kMode1AutoInc);
    if (!ok) {
        ESP_LOGE(TAG, "0x%02X: init failed", addr);
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "0x%02X: prescale %d, %.2f Hz", addr, prescale, freqHz_);
    return true;
}

void Pca9685::setPulseUs(int ch, float us) {
    long c = std::lround(us * freqHz_ * 4096.0f / 1e6f);
    uint16_t counts = static_cast<uint16_t>(std::clamp(c, 1L, 4095L));
    if (counts_[ch] != counts) {
        counts_[ch] = counts;
        dirty_ = true;
    }
}

void Pca9685::off(int ch) {
    if (counts_[ch] != 0) {
        counts_[ch] = 0;
        dirty_ = true;
    }
}

void Pca9685::flush() {
    if (!dev_ || !dirty_) return;
    // One auto-increment burst over all 16 channels: ON = 0, OFF = pulse end.
    uint8_t buf[1 + 4 * kChannels];
    buf[0] = reg::kLed0OnL;
    for (int ch = 0; ch < kChannels; ++ch) {
        uint8_t* p = &buf[1 + 4 * ch];
        p[0] = 0;
        p[1] = 0;
        p[2] = counts_[ch] & 0xFF;
        p[3] = counts_[ch] ? (counts_[ch] >> 8) : kFullOff;
    }
    if (i2c_master_transmit(dev_, buf, sizeof(buf), kTimeoutMs) == ESP_OK) {
        dirty_ = false;
    } else if (errors_++ % 100 == 0) {
        ESP_LOGW(TAG, "I2C write failed (%lu errors)", static_cast<unsigned long>(errors_));
    }
}

}  // namespace ss
