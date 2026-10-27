#include "core/Settings.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "core/Sequence.h"

namespace ss {

// ---------------------------------------------------------------------------
// Field tables

static const char* const kProfileNames[] = {"const", "trap", "tri", "sine", "bell"};
static const char* const kModeNames[] = {"once", "return", "pingpong"};
static const char* const kDriverNames[] = {"auto", "ledc", "mcpwm"};
static const char* const kPcfTypeNames[] = {"pcf8574", "pcf8575"};

static bool validateSequence(const char* v, std::string& msg) {
    Sequence seq;
    return parseSequence(v, seq, msg);
}

static bool validatePassword(const char* v, std::string& msg) {
    size_t n = std::strlen(v);
    if (n == 0 || (n >= 8 && n <= 63)) return true;
    msg = "password must be empty (open AP) or 8..63 characters";
    return false;
}

// The AP uses a /24 network with the ESP as gateway, so the host part must be 1..254 and
// the address must be a usable unicast one (otherwise DHCP fails and phones can't connect).
static bool validateIp(const char* v, std::string& msg) {
    unsigned a, b, c, d;
    char tail;
    if (std::sscanf(v, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 && c < 256 && d < 256) {
        if (a != 0 && a != 127 && a < 224 && d != 0 && d != 255) return true;
        msg = "not usable for the access point (needs a unicast address with host part 1..254, e.g. 192.168.4.1)";
        return false;
    }
    msg = "expected an IPv4 address like 192.168.4.1";
    return false;
}

#define FLD(S, member, name, type, mn, mx, flags) \
    {name, FieldType::type, offsetof(S, member), sizeof(S::member), mn, mx, nullptr, 0, flags, nullptr}
#define ENM(S, member, name, names, flags) \
    {name, FieldType::Enum, offsetof(S, member), 1, 0, 0, names, std::size(names), flags, nullptr}
#define VAL(S, member, name, type, flags, fn) \
    {name, FieldType::type, offsetof(S, member), sizeof(S::member), 0, 0, nullptr, 0, flags, fn}

static const FieldDesc kServoFields[] = {
    FLD(ServoConfig, name, "name", Str, 0, 0, 0),
    FLD(ServoConfig, pin, "pin", Pin, 0, 0, kReboot | kNoPcf),
    ENM(ServoConfig, driver, "driver", kDriverNames, kReboot),
    FLD(ServoConfig, enable, "enable", Bool, 0, 0, 0),
    FLD(ServoConfig, startDeg, "start_deg", F32, 0, 360, kMotion),
    FLD(ServoConfig, stopDeg, "stop_deg", F32, 0, 360, kMotion),
    FLD(ServoConfig, delayMs, "delay_ms", U32, 0, 3600000, kMotion),
    FLD(ServoConfig, durationMs, "duration_ms", U32, 0, 600000, kMotion),
    FLD(ServoConfig, dwellMs, "dwell_ms", U32, 0, 600000, kMotion),
    ENM(ServoConfig, profile, "profile", kProfileNames, kMotion),
    FLD(ServoConfig, accelPct, "accel_pct", F32, 0, 50, kMotion),
    ENM(ServoConfig, mode, "mode", kModeNames, kMotion),
    FLD(ServoConfig, minUs, "min_us", F32, 100, 3000, 0),
    FLD(ServoConfig, maxUs, "max_us", F32, 100, 3000, 0),
    FLD(ServoConfig, rangeDeg, "range_deg", F32, 1, 360, 0),
    FLD(ServoConfig, trimUs, "trim_us", F32, -500, 500, 0),
    FLD(ServoConfig, invert, "invert", Bool, 0, 0, 0),
    FLD(ServoConfig, maxDps, "max_dps", F32, 0, 10000, 0),
};

static const FieldDesc kSceneFields[] = {
    FLD(SceneConfig, periodMs, "period_ms", U32, 0, 3600000, kMotion),
    FLD(SceneConfig, autostart, "autostart", Bool, 0, 0, 0),
    FLD(SceneConfig, updateHz, "update_hz", U32, 10, 200, kReboot),
    FLD(SceneConfig, rejoinMs, "rejoin_ms", U32, 0, 10000, 0),
    FLD(SceneConfig, staggerMs, "stagger_ms", U32, 0, 2000, 0),
};

static const FieldDesc kPwmFields[] = {
    FLD(PwmConfig, ledcFreqHz, "ledc_freq_hz", U32, 40, 1000, kReboot),
    FLD(PwmConfig, mcpwmFreqHz, "mcpwm_freq_hz", U32, 40, 1000, kReboot),
};

static const FieldDesc kI2cFields[] = {
    FLD(I2cConfig, sda, "sda", Pin, 0, 0, kReboot | kGpioOnly),
    FLD(I2cConfig, scl, "scl", Pin, 0, 0, kReboot | kGpioOnly),
    FLD(I2cConfig, khz, "khz", U32, 10, 1000, kReboot),
};

static const FieldDesc kPcaFields[] = {
    FLD(PcaConfig, enable, "enable", Bool, 0, 0, kReboot),
    FLD(PcaConfig, addr, "addr", Hex, 0x40, 0x7F, kReboot),
    FLD(PcaConfig, freqHz, "freq_hz", U32, 24, 1526, kReboot),
    FLD(PcaConfig, oscHz, "osc_hz", U32, 20000000, 30000000, kReboot),
};

static const FieldDesc kButtonFields[] = {
    FLD(ButtonConfig, pin, "pin", Pin, 0, 0, kReboot | kGpioOnly),
    FLD(ButtonConfig, activeLow, "active_low", Bool, 0, 0, kReboot),
    FLD(ButtonConfig, pullup, "pullup", Bool, 0, 0, kReboot),
    FLD(ButtonConfig, debounceMs, "debounce_ms", U32, 0, 1000, 0),
};

static const FieldDesc kNetFields[] = {
    FLD(NetConfig, ssid, "AP/SSID", Str, 0, 0, kReboot),
    VAL(NetConfig, password, "AP/Password", Str, kReboot | kSecret, validatePassword),
    FLD(NetConfig, channel, "AP/Channel", U32, 1, 13, kReboot),
    VAL(NetConfig, ip, "AP/IP", Str, kReboot, validateIp),
    FLD(NetConfig, hostname, "Hostname", Str, 0, 0, kReboot),
    FLD(NetConfig, captive, "AP/Captive", Bool, 0, 0, kReboot),
};

static const FieldDesc kOutputFields[] = {
    FLD(OutputConfig, name, "name", Str, 0, 0, 0),
    FLD(OutputConfig, pin, "pin", Pin, 0, 0, kReboot | kNoPca),
    FLD(OutputConfig, enable, "enable", Bool, 0, 0, 0),
    FLD(OutputConfig, invert, "invert", Bool, 0, 0, 0),
    VAL(OutputConfig, seq, "seq", Str, 0, validateSequence),
};

static const FieldDesc kPcfFields[] = {
    FLD(PcfConfig, enable, "enable", Bool, 0, 0, kReboot),
    ENM(PcfConfig, type, "type", kPcfTypeNames, kReboot),
    FLD(PcfConfig, addr, "addr", Hex, 0x20, 0x3F, kReboot),
};

#define GRP(name, count, member, S, fields, pinField) \
    {name, count, offsetof(Config, member), sizeof(S), fields, static_cast<int>(std::size(fields)), pinField}

static const GroupDesc kGroups[] = {
    GRP("servo", kMaxServos, servo, ServoConfig, kServoFields, 1),
    GRP("out", kMaxOutputs, out, OutputConfig, kOutputFields, 1),
    GRP("scene", 0, scene, SceneConfig, kSceneFields, -1),
    GRP("pwm", 0, pwm, PwmConfig, kPwmFields, -1),
    GRP("i2c", 0, i2c, I2cConfig, kI2cFields, -1),
    GRP("pca", kMaxPca, pca, PcaConfig, kPcaFields, -1),
    GRP("pcf", kMaxPcf, pcf, PcfConfig, kPcfFields, -1),
    GRP("button", 0, button, ButtonConfig, kButtonFields, -1),
    GRP(nullptr, 0, net, NetConfig, kNetFields, -1),  // named settings ($AP/SSID, ...)
};

// ---------------------------------------------------------------------------
// Helpers

static bool ieq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    }
    return *a == *b;
}

static std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string formatFloat(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    char* end = buf + std::strlen(buf) - 1;
    while (*end == '0') *end-- = 0;
    if (*end == '.') *end = 0;
    if (std::strcmp(buf, "-0") == 0) return "0";
    return buf;
}

static bool parseNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == 0 && std::isfinite(out);
}

static bool parseUnsigned(const std::string& s, int base, double& out) {
    if (s.empty() || s[0] == '-') return false;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, base);
    if (!end || *end != 0) return false;
    out = static_cast<double>(v);
    return true;
}

static bool parseBool(const std::string& s, bool& out) {
    std::string l = lower(s);
    if (l == "true" || l == "1" || l == "on" || l == "yes") return out = true, true;
    if (l == "false" || l == "0" || l == "off" || l == "no") return out = false, true;
    return false;
}

bool Settings::parsePin(const std::string& s, PinSpec& pin) {
    std::string l = lower(s);
    unsigned a, b;
    char tail;
    if (l == "none" || l.empty()) {
        pin = pinNone();
        return true;
    }
    if (std::sscanf(l.c_str(), "gpio.%u%c", &a, &tail) == 1 && a < 40) {
        pin = pinGpio(static_cast<uint8_t>(a));
        return true;
    }
    if (std::sscanf(l.c_str(), "pcf.%u.%u%c", &a, &b, &tail) == 2 && a < kMaxPcf && b < 16) {
        pin = pinPcf(static_cast<uint8_t>(a), static_cast<uint8_t>(b));
        return true;
    }
    if (std::sscanf(l.c_str(), "pca.%u.%u%c", &a, &b, &tail) == 2 && a < kMaxPca && b < kPcaChannels) {
        pin = pinPca(static_cast<uint8_t>(a), static_cast<uint8_t>(b));
        return true;
    }
    return false;
}

size_t utf8SequenceLength(const std::string& s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<uint8_t>(s[k]); };
    const uint8_t c = b(i);
    if (c < 0x80) return 1;
    size_t n;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) n = 2, cp = c & 0x1F;
    else if (c >= 0xE0 && c <= 0xEF) n = 3, cp = c & 0x0F;
    else if (c >= 0xF0 && c <= 0xF4) n = 4, cp = c & 0x07;
    else return 0;
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        if ((b(i + k) & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b(i + k) & 0x3F);
    }
    if ((n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return 0;
    return n;
}

void makeValidUtf8(std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8SequenceLength(s, i);
        if (n == 0) s[i++] = '?';
        else i += n;
    }
}

// Names, SSID and the like end up in WebSocket text frames, the UI and the AP beacon:
// UTF-8 only (a Latin-1 serial terminal would send other bytes), no control characters.
static bool validText(const std::string& v, std::string& msg) {
    for (size_t i = 0; i < v.size();) {
        const size_t n = utf8SequenceLength(v, i);
        const uint8_t c = static_cast<uint8_t>(v[i]);
        if (n == 0 || c < 0x20 || c == 0x7F) return msg = "expected UTF-8 text without control characters", false;
        i += n;
    }
    return true;
}

std::string Settings::formatPin(const PinSpec& pin) {
    char buf[16];
    switch (pin.type) {
        case PinType::Gpio:
            std::snprintf(buf, sizeof(buf), "gpio.%u", pin.num);
            return buf;
        case PinType::Pca:
            std::snprintf(buf, sizeof(buf), "pca.%u.%u", pin.board, pin.num);
            return buf;
        case PinType::Pcf:
            std::snprintf(buf, sizeof(buf), "pcf.%u.%u", pin.board, pin.num);
            return buf;
        default:
            return "none";
    }
}

// ---------------------------------------------------------------------------
// Settings

bool Settings::unwired(const GroupDesc& g, int index) const {
    if (g.pinField < 0) return false;
    SettingRef ref{&g, index, &g.fields[g.pinField], nullptr};
    return reinterpret_cast<const PinSpec*>(ptr(ref, static_cast<const Config&>(cfg_)))->type == PinType::None;
}

uint8_t* Settings::ptr(const SettingRef& ref, Config& cfg) const {
    return reinterpret_cast<uint8_t*>(&cfg) + ref.group->base + ref.index * ref.group->stride + ref.field->offset;
}

const uint8_t* Settings::ptr(const SettingRef& ref, const Config& cfg) const {
    return reinterpret_cast<const uint8_t*>(&cfg) + ref.group->base + ref.index * ref.group->stride +
           ref.field->offset;
}

bool Settings::find(const std::string& path, SettingRef& ref) const {
    ref = SettingRef();
    if (path.empty()) return false;

    if (path[0] != '/') {  // named setting
        for (const GroupDesc& g : kGroups) {
            if (g.name) continue;
            for (int f = 0; f < g.fieldCount; ++f) {
                if (ieq(g.fields[f].name, path.c_str())) {
                    ref.group = &g;
                    ref.field = &g.fields[f];
                    ref.named = g.fields[f].name;
                    return true;
                }
            }
        }
        return false;
    }

    // "/group[/index]/field"
    char seg[3][24] = {};
    int n = 0;
    size_t pos = 1;
    while (pos <= path.size() && n < 3) {
        size_t next = path.find('/', pos);
        if (next == std::string::npos) next = path.size();
        if (next - pos >= sizeof(seg[0])) return false;
        path.copy(seg[n++], next - pos, pos);
        pos = next + 1;
    }
    if (pos <= path.size()) return false;  // too many segments

    for (const GroupDesc& g : kGroups) {
        if (!g.name || !ieq(g.name, seg[0])) continue;
        const char* fieldName;
        if (g.count > 0) {
            if (n != 3) return false;
            char* end = nullptr;
            long idx = std::strtol(seg[1], &end, 10);
            if (!*seg[1] || *end || idx < 0 || idx >= g.count) return false;
            ref.index = static_cast<int>(idx);
            fieldName = seg[2];
        } else {
            if (n != 2) return false;
            fieldName = seg[1];
        }
        for (int f = 0; f < g.fieldCount; ++f) {
            if (ieq(g.fields[f].name, fieldName)) {
                ref.group = &g;
                ref.field = &g.fields[f];
                return true;
            }
        }
        return false;
    }
    return false;
}

std::string Settings::path(const SettingRef& ref) const {
    if (ref.named) return ref.named;
    std::string p = "/";
    p += ref.group->name;
    if (ref.group->count > 0) p += "/" + std::to_string(ref.index);
    p += "/";
    p += ref.field->name;
    return p;
}

std::string Settings::get(const SettingRef& ref, bool maskSecret) const {
    const uint8_t* p = ptr(ref, static_cast<const Config&>(cfg_));
    const FieldDesc& f = *ref.field;
    switch (f.type) {
        case FieldType::Bool:
            return *reinterpret_cast<const bool*>(p) ? "true" : "false";
        case FieldType::U32:
            return std::to_string(*reinterpret_cast<const uint32_t*>(p));
        case FieldType::Hex: {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%02lX", static_cast<unsigned long>(*reinterpret_cast<const uint32_t*>(p)));
            return buf;
        }
        case FieldType::F32:
            return formatFloat(*reinterpret_cast<const float*>(p));
        case FieldType::Str: {
            const char* s = reinterpret_cast<const char*>(p);
            if (maskSecret && (f.flags & kSecret) && *s) return "********";
            return std::string(s, strnlen(s, f.size));
        }
        case FieldType::Enum: {
            uint8_t v = *p;
            return v < f.enumCount ? f.enumNames[v] : "?";
        }
        case FieldType::Pin:
            return formatPin(*reinterpret_cast<const PinSpec*>(p));
    }
    return "";
}

Err Settings::set(const SettingRef& ref, const std::string& value, std::string& msg) {
    uint8_t* p = ptr(ref, cfg_);
    const FieldDesc& f = *ref.field;
    double num = 0;

    auto rangeCheck = [&]() {
        if (num < f.min || num > f.max) {
            msg = "allowed range " + formatFloat(f.min) + ".." + formatFloat(f.max);
            return false;
        }
        return true;
    };

    switch (f.type) {
        case FieldType::Bool: {
            bool b;
            if (!parseBool(value, b)) return msg = "expected true or false", Err::BadValue;
            *reinterpret_cast<bool*>(p) = b;
            break;
        }
        case FieldType::U32:
        case FieldType::Hex:
            if (!parseUnsigned(value, f.type == FieldType::Hex ? 0 : 10, num)) return msg = "expected a whole number", Err::BadValue;
            if (!rangeCheck()) return Err::OutOfRange;
            *reinterpret_cast<uint32_t*>(p) = static_cast<uint32_t>(num);
            break;
        case FieldType::F32:
            if (!parseNumber(value, num)) return msg = "expected a number", Err::BadValue;
            if (!rangeCheck()) return Err::OutOfRange;
            *reinterpret_cast<float*>(p) = static_cast<float>(num);
            break;
        case FieldType::Str:
            if (value.size() >= f.size) return msg = "at most " + std::to_string(f.size - 1) + " bytes", Err::OutOfRange;
            if (!validText(value, msg)) return Err::BadValue;
            if (f.validate && !f.validate(value.c_str(), msg)) return Err::BadValue;
            std::memset(p, 0, f.size);
            value.copy(reinterpret_cast<char*>(p), value.size());
            break;
        case FieldType::Enum:
            for (uint8_t i = 0; i < f.enumCount; ++i) {
                if (ieq(f.enumNames[i], value.c_str())) {
                    *p = i;
                    return Err::Ok;
                }
            }
            msg = "expected one of:";
            for (uint8_t i = 0; i < f.enumCount; ++i) msg += std::string(" ") + f.enumNames[i];
            return Err::BadValue;
        case FieldType::Pin: {
            PinSpec pin;
            if (!parsePin(value, pin)) {
                msg = (f.flags & kGpioOnly) ? "expected none or gpio.N"
                      : (f.flags & kNoPca) ? "expected none, gpio.N or pcf.B.C"
                                           : "expected none, gpio.N or pca.B.C";
                return Err::BadValue;
            }
            if ((f.flags & kGpioOnly) && pin.type != PinType::Gpio && pin.type != PinType::None) {
                return msg = "only gpio.N or none", Err::BadValue;
            }
            if ((f.flags & kNoPca) && pin.type == PinType::Pca) {
                return msg = "digital outputs use gpio.N or pcf.B.C", Err::BadValue;
            }
            if ((f.flags & kNoPcf) && pin.type == PinType::Pcf) return msg = "servos use gpio.N or pca.B.C", Err::BadValue;
            *reinterpret_cast<PinSpec*>(p) = pin;
            break;
        }
    }
    return Err::Ok;
}

bool Settings::equals(const SettingRef& ref, const Config& other) const {
    const uint8_t* cur = ptr(ref, static_cast<const Config&>(cfg_));
    const uint8_t* oth = ptr(ref, other);
    if (ref.field->type == FieldType::Str) {
        return std::strncmp(reinterpret_cast<const char*>(cur), reinterpret_cast<const char*>(oth), ref.field->size) == 0;
    }
    if (ref.field->type == FieldType::Pin) {
        return *reinterpret_cast<const PinSpec*>(cur) == *reinterpret_cast<const PinSpec*>(oth);
    }
    return std::memcmp(cur, oth, ref.field->size) == 0;
}

void Settings::forEachAll(const std::function<void(const SettingRef&)>& fn) const {
    for (const GroupDesc& g : kGroups) {
        int count = g.count > 0 ? g.count : 1;
        for (int i = 0; i < count; ++i) {
            for (int f = 0; f < g.fieldCount; ++f) {
                SettingRef ref;
                ref.group = &g;
                ref.index = i;
                ref.field = &g.fields[f];
                ref.named = g.name ? nullptr : g.fields[f].name;
                fn(ref);
            }
        }
    }
}

// True if `path` lies inside `prefix` (whole segments, case-insensitive).
static bool underPrefix(const std::string& path, const std::string& prefix) {
    if (prefix.empty()) return true;
    if (path.size() < prefix.size()) return false;
    if (lower(path.substr(0, prefix.size())) != lower(prefix)) return false;
    return path.size() == prefix.size() || path[prefix.size()] == '/' || prefix.back() == '/';
}

void Settings::forEach(const std::string& prefix, const std::function<void(const SettingRef&)>& fn) const {
    for (const GroupDesc& g : kGroups) {
        int count = g.count > 0 ? g.count : 1;
        for (int i = 0; i < count; ++i) {
            for (int f = 0; f < g.fieldCount; ++f) {
                SettingRef ref;
                ref.group = &g;
                ref.index = i;
                ref.field = &g.fields[f];
                ref.named = g.name ? nullptr : g.fields[f].name;
                std::string p = path(ref);
                if (!underPrefix(p, prefix)) continue;
                // Unwired servo/output slots only show up when asked for explicitly.
                if (unwired(g, i) && prefix.size() <= std::string(g.name).size() + 1) continue;
                fn(ref);
            }
        }
    }
}

std::string Settings::dumpChanged() const {
    std::string out;
    forEachAll([&](const SettingRef& ref) {
        if (!isDefault(ref)) out += "$" + path(ref) + "=" + get(ref) + "\n";
    });
    return out;
}

std::string Settings::dumpYaml() const {
    std::string out;
    for (const GroupDesc& g : kGroups) {
        if (!g.name) continue;
        out += std::string(g.name) + ":\n";
        int count = g.count > 0 ? g.count : 1;
        for (int i = 0; i < count; ++i) {
            if (unwired(g, i)) continue;
            std::string indent = "  ";
            if (g.count > 0) {
                out += "  " + std::to_string(i) + ":\n";
                indent = "    ";
            }
            for (int f = 0; f < g.fieldCount; ++f) {
                SettingRef ref{&g, i, &g.fields[f], nullptr};
                out += indent + g.fields[f].name + ": " + get(ref, true) + "\n";
            }
        }
    }
    return out;
}

}  // namespace ss
