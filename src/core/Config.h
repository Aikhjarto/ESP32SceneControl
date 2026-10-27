// Plain configuration data for the servo scene. Portable (no ESP-IDF headers),
// so it is shared by the firmware and the native unit tests.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ss {

constexpr int kMaxServos = 64;
constexpr int kMaxPca = 4;
constexpr int kMaxOutputs = 64;      // digital outputs
constexpr int kMaxPcf = 8;           // PCF8574/PCF8575 port expanders
constexpr int kMaxSwitchPoints = 16; // per digital output sequence
constexpr int kPcaChannels = 16;

enum class PinType : uint8_t { None, Gpio, Pca, Pcf };

// "none", "gpio.13", "pca.<board>.<channel>" (servos) or "pcf.<board>.<bit>" (digital outputs)
struct PinSpec {
    PinType type;
    uint8_t board;  // PCA9685 / PCF857x board index
    uint8_t num;    // GPIO number, PCA channel or PCF bit

    bool operator==(const PinSpec& o) const { return type == o.type && board == o.board && num == o.num; }
    bool operator!=(const PinSpec& o) const { return !(*this == o); }
};

constexpr PinSpec pinNone() { return {PinType::None, 0, 0}; }
constexpr PinSpec pinGpio(uint8_t n) { return {PinType::Gpio, 0, n}; }
constexpr PinSpec pinPca(uint8_t board, uint8_t ch) { return {PinType::Pca, board, ch}; }
constexpr PinSpec pinPcf(uint8_t board, uint8_t bit) { return {PinType::Pcf, board, bit}; }

enum class Profile : uint8_t { Const, Trap, Tri, Sine, Bell, Count };
enum class Mode : uint8_t { Once, Return, PingPong, Count };
enum class Driver : uint8_t { Auto, Ledc, Mcpwm, Count };

struct ServoConfig {
    char name[16];
    PinSpec pin;
    Driver driver;
    bool enable;
    float startDeg;
    float stopDeg;
    uint32_t delayMs;
    uint32_t durationMs;
    uint32_t dwellMs;
    Profile profile;
    float accelPct;  // trapezoid: share of the duration spent accelerating (and decelerating)
    Mode mode;
    float minUs;     // pulse at 0 deg
    float maxUs;     // pulse at rangeDeg
    float rangeDeg;
    float trimUs;
    bool invert;
    float maxDps;    // servo speed limit in deg/s, 0 = unlimited
};

struct SceneConfig {
    uint32_t periodMs;  // 0 = automatic (longest servo cycle)
    bool autostart;
    uint32_t updateHz;
    uint32_t rejoinMs;  // minimum duration of a rejoin blend
    uint32_t staggerMs; // delay between enabling servos at boot
};

struct PwmConfig {
    uint32_t ledcFreqHz;
    uint32_t mcpwmFreqHz;
};

struct I2cConfig {
    PinSpec sda;
    PinSpec scl;
    uint32_t khz;
};

struct PcaConfig {
    bool enable;
    uint32_t addr;
    uint32_t freqHz;
    uint32_t oscHz;  // internal oscillator, nominally 25 MHz; calibrate for exact pulse widths
};

struct ButtonConfig {
    PinSpec pin;
    bool activeLow;
    bool pullup;
    uint32_t debounceMs;
};

struct NetConfig {
    char ssid[33];  // empty = "SceneControl-XXXX" from the MAC address
    char password[65];
    uint32_t channel;
    char ip[16];
    char hostname[33];
    bool captive;
};

// Digital output: switches on/off at the points of its sequence, every scene cycle.
struct OutputConfig {
    char name[16];
    PinSpec pin;   // gpio.N or pcf.B.C
    bool enable;
    bool invert;   // output level low when "on" (active-low LEDs, relay boards)
    char seq[200]; // switch points "ms:state,..." within the scene cycle, e.g. "0:on,500:off,900:blink"
                   // (see Sequence.h; fits kMaxSwitchPoints on/off points with 7-digit times)
};

enum class PcfType : uint8_t { Pcf8574, Pcf8575, Count };

struct PcfConfig {
    bool enable;
    PcfType type;
    uint32_t addr;
};

struct Config {
    ServoConfig servo[kMaxServos];
    OutputConfig out[kMaxOutputs];
    SceneConfig scene;
    PwmConfig pwm;
    I2cConfig i2c;
    PcaConfig pca[kMaxPca];
    PcfConfig pcf[kMaxPcf];
    ButtonConfig button;
    NetConfig net;
};

void setDefaults(Config& cfg);
const Config& defaultConfig();

// Digital output is wired to something and switched on.
inline bool outputActive(const OutputConfig& o) { return o.enable && o.pin.type != PinType::None; }

// Servo is wired to something and switched on.
inline bool servoActive(const ServoConfig& s) { return s.enable && s.pin.type != PinType::None; }

}  // namespace ss
