// PCF8574 (8 bit) / PCF8575 (16 bit) quasi-bidirectional port expander used for digital
// outputs: writing 1 gives a weak pull-up (high), 0 a strong low. The whole port is written
// in one transfer when a bit changed, on the first flush, and about once a second anyway:
// the chip keeps its state across an ESP restart (and resets to all-high on a brown-out),
// so the firmware never relies on what it last wrote.
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
    void flush();                  // call every motion tick

private:
    i2c_master_dev_handle_t dev_ = nullptr;
    bool sixteen_ = false;
    uint16_t port_ = 0xFFFF;  // power-on state: all high
    bool dirty_ = true;     // write the whole port on the first flush
    uint32_t idleFlushes_ = 0;
    uint32_t errors_ = 0;
};

}  // namespace ss
