// Settings tree over Config, addressed FluidNC-style:
//   tree settings   $/servo/3/start_deg=20   $/scene/period_ms   $/pca/0/addr
//   named settings  $AP/SSID=...  $Hostname=...
// Each field is described once (type, range, flags); paths resolve against those tables.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/Config.h"

namespace ss {

enum class FieldType : uint8_t { Bool, U32, Hex, F32, Str, Enum, Pin };

enum FieldFlag : uint8_t {
    kReboot = 1,    // takes effect after a restart
    kMotion = 2,    // changes the choreography: servo(s) rejoin smoothly
    kGpioOnly = 4,  // pin field that only accepts gpio.N or none
    kSecret = 8,    // masked in listings
    kNoPca = 16,    // pin field of a digital output: gpio.N or pcf.B.C
    kNoPcf = 32,    // pin field of a servo: gpio.N or pca.B.C
};

struct FieldDesc {
    const char* name;
    FieldType type;
    uint16_t offset;
    uint16_t size;
    double min;
    double max;
    const char* const* enumNames;
    uint8_t enumCount;
    uint8_t flags;
    bool (*validate)(const char* value, std::string& msg);
};

struct GroupDesc {
    const char* name;  // tree path segment; nullptr for groups only reachable via named settings
    int count;         // 0 = single instance, N = indexed /name/<0..N-1>/
    size_t base;
    size_t stride;
    const FieldDesc* fields;
    int fieldCount;
    int pinField;  // index of the pin field: slots with pin none are hidden in listings; -1 = none
};

struct SettingRef {
    const GroupDesc* group = nullptr;
    int index = 0;
    const FieldDesc* field = nullptr;
    const char* named = nullptr;  // set for named settings such as "AP/SSID"
    bool valid() const { return field != nullptr; }
};

enum class Err : int {
    Ok = 0,
    BadValue = 2,
    UnknownCommand = 3,
    OutOfRange = 4,
    UnknownSetting = 5,
    NoSuchServo = 6,
    FlashFailed = 7,
};

class Settings {
public:
    explicit Settings(Config& cfg) : cfg_(cfg) {}

    // path is what follows '$': "/servo/3/start_deg" or "AP/SSID" (case-insensitive).
    bool find(const std::string& path, SettingRef& ref) const;
    std::string path(const SettingRef& ref) const;  // without '$'
    std::string get(const SettingRef& ref, bool maskSecret = false) const;
    Err set(const SettingRef& ref, const std::string& value, std::string& msg);
    bool isDefault(const SettingRef& ref) const { return equals(ref, defaultConfig()); }
    bool equals(const SettingRef& ref, const Config& other) const;  // same value in `other`?

    // Visits every setting whose path starts with prefix (segment-wise). Unwired servo slots
    // (pin none) are skipped unless addressed explicitly by the prefix.
    void forEach(const std::string& prefix, const std::function<void(const SettingRef&)>& fn) const;
    // Visits every setting, including unwired servo slots.
    void forEachAll(const std::function<void(const SettingRef&)>& fn) const;

    // "$/path=value" lines for every setting that differs from its default (what gets saved).
    std::string dumpChanged() const;
    // FluidNC-like YAML view of the whole configuration ($CD).
    std::string dumpYaml() const;

    static bool parsePin(const std::string& s, PinSpec& pin);
    static std::string formatPin(const PinSpec& pin);

private:
    bool unwired(const GroupDesc& g, int index) const;
    uint8_t* ptr(const SettingRef& ref, Config& cfg) const;
    const uint8_t* ptr(const SettingRef& ref, const Config& cfg) const;

    Config& cfg_;
};

}  // namespace ss
