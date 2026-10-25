#include "core/Config.h"

#include <cstring>

namespace ss {

// Free output pins on an ESP32-WROOM DevKit (no flash, UART0, I2C or input-only pins).
static const uint8_t kDefaultServoGpios[] = {2, 4, 5, 13, 14, 15, 16, 17, 18, 19, 23, 25, 26, 27, 32, 33};
constexpr int kDefaultGpioServos = sizeof(kDefaultServoGpios) / sizeof(kDefaultServoGpios[0]);

void setDefaults(Config& cfg) {
    std::memset(&cfg, 0, sizeof(cfg));

    for (int i = 0; i < kMaxServos; ++i) {
        ServoConfig& s = cfg.servo[i];
        if (i < kDefaultGpioServos) {
            s.pin = pinGpio(kDefaultServoGpios[i]);
        } else if (i < kDefaultGpioServos + kPcaChannels) {
            s.pin = pinPca(0, i - kDefaultGpioServos);
        } else {
            s.pin = pinNone();
        }
        s.driver = Driver::Auto;
        s.enable = s.pin.type != PinType::None;
        s.startDeg = 45;
        s.stopDeg = 135;
        s.delayMs = (i % 32) * 250;  // a travelling wave as demo choreography
        s.durationMs = 1500;
        s.dwellMs = 500;
        s.profile = Profile::Bell;
        s.accelPct = 25;
        s.mode = Mode::Return;
        s.minUs = 500;  // MG90S
        s.maxUs = 2400;
        s.rangeDeg = 180;
        s.trimUs = 0;
        s.invert = false;
        s.maxDps = 600;
    }

    cfg.scene = {0, true, 50, 600, 50};
    cfg.pwm = {50, 50};
    cfg.i2c = {pinGpio(21), pinGpio(22), 400};
    for (int b = 0; b < kMaxPca; ++b) {
        cfg.pca[b] = {b == 0, static_cast<uint32_t>(0x40 + b), 50, 25000000};
    }
    cfg.button = {pinGpio(0), true, true, 30};  // BOOT button on DevKit boards

    NetConfig& n = cfg.net;
    std::strcpy(n.password, "servoscene");
    n.channel = 6;
    std::strcpy(n.ip, "192.168.4.1");
    std::strcpy(n.hostname, "servoscene");
    n.captive = true;
}

const Config& defaultConfig() {
    static Config cfg;
    static bool init = false;
    if (!init) {
        setDefaults(cfg);
        init = true;
    }
    return cfg;
}

}  // namespace ss
