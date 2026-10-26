// PCF8574 (8 bit) / PCF8575 (16 bit) quasi-bidirectional port expander used for digital
// outputs: writing 1 gives a weak pull-up (high), 0 a strong low. The whole port is written
// in one transfer, only when a bit changed.
#pragma once

#include <cstdint>

#include "driver/i2c_master.h"

namespace ss {

class Pcf857x {
public:
    bool begin(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t sclHz, bool sixteenBit);
    bool ok() const { return dev_ != nullptr; }
    int bits() const { return sixteen_ ? 16 : 8; }

    void set(int bit, bool high);  // buffered; written by flush()
    void flush();

private:
    i2c_master_dev_handle_t dev_ = nullptr;
    bool sixteen_ = false;
    uint16_t port_ = 0xFFFF;  // power-on state: all high
    bool dirty_ = false;
    uint32_t errors_ = 0;
};

}  // namespace ss
