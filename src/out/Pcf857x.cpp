#include "out/Pcf857x.h"

#include "esp_log.h"

namespace ss {

static const char* TAG = "pcf857x";

constexpr int kTimeoutMs = 20;
constexpr uint32_t kRefreshFlushes = 50;  // rewrite unchanged ports after this many flushes (~1 s)

bool Pcf857x::begin(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t sclHz, bool sixteenBit) {
    if (i2c_master_probe(bus, addr, kTimeoutMs) != ESP_OK) return false;
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = addr;
    dc.scl_speed_hz = sclHz;
    if (i2c_master_bus_add_device(bus, &dc, &dev_) != ESP_OK) {
        dev_ = nullptr;
        return false;
    }
    sixteen_ = sixteenBit;
    ESP_LOGI(TAG, "0x%02X: %s", addr, sixteenBit ? "PCF8575, 16 bit" : "PCF8574, 8 bit");
    return true;
}

void Pcf857x::set(int bit, bool high) {
    const uint16_t mask = static_cast<uint16_t>(1u << bit);
    const uint16_t next = high ? (port_ | mask) : (port_ & ~mask);
    if (next != port_) {
        port_ = next;
        dirty_ = true;
    }
}

void Pcf857x::flush() {
    if (!dev_) return;
    if (!dirty_ && ++idleFlushes_ < kRefreshFlushes) return;
    idleFlushes_ = 0;
    const uint8_t buf[2] = {static_cast<uint8_t>(port_ & 0xFF), static_cast<uint8_t>(port_ >> 8)};  // P0x, then P1x
    if (i2c_master_transmit(dev_, buf, sixteen_ ? 2 : 1, kTimeoutMs) == ESP_OK) {
        dirty_ = false;
    } else if (errors_++ % 100 == 0) {
        ESP_LOGW(TAG, "I2C write failed (%lu errors)", static_cast<unsigned long>(errors_));
    }
}

}  // namespace ss
