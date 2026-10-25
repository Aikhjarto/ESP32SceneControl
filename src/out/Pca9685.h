// PCA9685 16-channel PWM expander on the new ESP-IDF I2C master driver.
#pragma once

#include <cstdint>

#include "driver/i2c_master.h"

namespace ss {

class Pca9685 {
public:
    static constexpr int kChannels = 16;

    bool begin(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t sclHz, uint32_t freqHz, uint32_t oscHz);
    bool ok() const { return dev_ != nullptr; }
    float freqHz() const { return freqHz_; }

    // Buffered; written to the chip by flush().
    void setPulseUs(int ch, float us);
    void off(int ch);
    void flush();

private:
    bool writeReg(uint8_t reg, uint8_t value);

    i2c_master_dev_handle_t dev_ = nullptr;
    float freqHz_ = 50;
    uint16_t counts_[kChannels] = {};  // 0 = full off
    bool dirty_ = false;
    uint32_t errors_ = 0;
};

}  // namespace ss
