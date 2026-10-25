// Maps logical servos onto the PWM hardware: LEDC (high- and low-speed),
// both MCPWM units and optional PCA9685 boards.
#pragma once

#include <string>

#include "core/Config.h"
#include "driver/i2c_master.h"
#include "driver/mcpwm_prelude.h"
#include "out/Pca9685.h"
#include "soc/soc_caps.h"

namespace ss {

enum class HwKind : uint8_t { None, LedcHs, LedcLs, Mcpwm0, Mcpwm1, Pca };

struct HwChannel {
    HwKind kind = HwKind::None;
    uint8_t unit = 0;  // PCA board
    uint8_t ch = 0;    // channel within the block
};

class Outputs {
public:
    // Allocates and initializes the hardware for every servo with a pin.
    // Problems (bad pins, missing PCA boards, exhausted blocks) are appended to `log`, one per line.
    void begin(const Config& cfg, std::string& log);

    // Called from the motion task only.
    void setPulseUs(int servo, float us);
    void off(int servo);
    void flush();  // pushes buffered PCA9685 values over I2C

    const HwChannel& channel(int servo) const { return map_[servo]; }
    bool pcaFound(int board) const { return pca_[board].ok(); }
    std::string describe(int servo) const;  // e.g. "mcpwm0.3", "ledc-hs.5", "pca0.12"
    int used(HwKind k) const { return k >= HwKind::LedcHs && k <= HwKind::Mcpwm1 ? used_[static_cast<int>(k) - 1] : 0; }

    static constexpr int kMcpwmPerUnit = SOC_MCPWM_OPERATORS_PER_GROUP * SOC_MCPWM_GENERATORS_PER_OPERATOR;

    static bool gpioUsableForServo(int gpio);

private:
    struct McpwmUnit {
        mcpwm_timer_handle_t timer = nullptr;
        mcpwm_oper_handle_t oper[3] = {};
        mcpwm_cmpr_handle_t cmp[6] = {};
        mcpwm_gen_handle_t gen[6] = {};
        float ticksPerUs = 0;
        bool forcedOff[6] = {};
    };

    bool allocate(const Config& cfg, int servo, std::string& log);
    void initLedc(const Config& cfg);
    void initMcpwm(const Config& cfg, int unit, std::string& log);
    void initPca(const Config& cfg, std::string& log);

    HwChannel map_[kMaxServos];
    PinSpec pin_[kMaxServos] = {};
    int used_[4] = {};  // LEDC-HS, LEDC-LS, MCPWM0, MCPWM1 (HwKind - 1)
    int rr_ = 0;        // round-robin cursor for Driver::Auto
    uint32_t ledcFreq_ = 50;
    McpwmUnit mcpwm_[2];
    i2c_master_bus_handle_t i2c_ = nullptr;
    Pca9685 pca_[kMaxPca];
};

}  // namespace ss
