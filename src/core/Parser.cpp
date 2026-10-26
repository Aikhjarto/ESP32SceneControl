#include "core/Parser.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <memory>

#include "core/Motion.h"

namespace ss {

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static bool ieq(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return i == a.size() && !b[i];
}

static std::string fmt(const char* f, double a, double b = 0, double c = 0) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), f, a, b, c);
    return buf;
}

void Parser::execute(const std::string& raw, ReplySink& out) {
    std::string msg;
    Err e = run(trim(raw), out, msg);
    if (e == Err::Ok) {
        out.line("ok");
    } else {
        if (!msg.empty()) out.line("[MSG:ERR: " + msg + "]");
        out.line("error:" + std::to_string(static_cast<int>(e)));
    }
}

void Parser::realtime(char c, ReplySink& out) {
    switch (c) {
        case '!':
            host_.setRunning(false, "command");
            break;
        case '~':
            host_.setRunning(true, "command");
            break;
        case '?': {
            Guard g(host_);
            out.line(scene_.status());
            break;
        }
        case 0x18: {
            Guard g(host_);
            scene_.reset();
            out.line("[MSG:INFO: Scene reset]");
            break;
        }
        default:
            break;
    }
}

void Parser::replay(const std::string& text, ReplySink& out) {
    // Reports only failing lines (with the reason), so a clean replay is silent.
    struct ErrorSink : ReplySink {
        std::string reason;
        bool failed = false;
        void line(const std::string& t) override {
            if (t.rfind("error:", 0) == 0) failed = true;
            if (t.rfind("[MSG:ERR: ", 0) == 0) reason = t.substr(10, t.size() - 11);
        }
    };
    replaying_ = true;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty()) continue;
        ErrorSink sink;
        execute(line, sink);
        if (sink.failed) out.line("[MSG:WARN: Config line failed: " + line + " (" + sink.reason + ")]");
    }
    replaying_ = false;
}

Err Parser::run(const std::string& line, ReplySink& out, std::string& msg) {
    if (line.empty()) return Err::Ok;
    if (line[0] != '$') {
        msg = "commands start with $ (try $H)";
        return Err::UnknownCommand;
    }
    std::string body = line.substr(1);
    if (body.empty() || ieq(body, "H") || ieq(body, "Help")) {
        help(out);
        return Err::Ok;
    }
    if (body == "$") {
        listSettings("", out);
        return Err::Ok;
    }

    size_t eq = body.find('=');
    bool hasValue = eq != std::string::npos;
    std::string name = trim(body.substr(0, eq));
    std::string value = hasValue ? trim(body.substr(eq + 1)) : "";

    if (!name.empty() && name[0] == '/') return setting(name, value, hasValue, out, msg);

    Err e = command(name, value, hasValue, out, msg);
    if (e != Err::UnknownCommand) return e;

    SettingRef ref;
    if (settings_.find(name, ref)) return setting(name, value, hasValue, out, msg);
    msg = "unknown command or setting: $" + name;
    return Err::UnknownCommand;
}

Err Parser::setting(const std::string& path, const std::string& value, bool hasValue, ReplySink& out,
                    std::string& msg) {
    Guard g(host_);
    SettingRef ref;
    if (!settings_.find(path, ref)) {
        if (hasValue) {
            msg = "unknown setting: $" + path;
            return Err::UnknownSetting;
        }
        // Query of a subtree, e.g. $/servo/3 or $/scene
        bool any = false;
        settings_.forEach(path, [&](const SettingRef& r) {
            out.line("$" + settings_.path(r) + "=" + settings_.get(r, true));
            any = true;
        });
        if (!any) {
            msg = "unknown setting: $" + path;
            return Err::UnknownSetting;
        }
        return Err::Ok;
    }
    if (!hasValue) {
        out.line("$" + settings_.path(ref) + "=" + settings_.get(ref, true));
        return Err::Ok;
    }
    if (settings_.get(ref) == value) return Err::Ok;
    Err e = settings_.set(ref, value, msg);
    if (e == Err::Ok) afterChange(ref, out);
    return e;
}

void Parser::afterChange(const SettingRef& ref, ReplySink& out) {
    const bool servoField = ref.group->name && ref.group->count == kMaxServos;
    // The edited servo blends onto its new trajectory. A changed scene period is noticed by
    // the scene itself, which switches return-mode servos over smoothly.
    if ((ref.field->flags & kMotion) && servoField) scene_.rejoin(ref.index);
    if (replaying_) return;

    if (ref.field->flags & kReboot) out.line("[MSG:INFO: Restart required to apply $" + settings_.path(ref) + "]");
    if (!servoField) {
        if (ref.field->offset == offsetof(SceneConfig, periodMs) && ref.group->base == offsetof(Config, scene)) {
            warnShortPeriod(out);
        }
        return;
    }

    const ServoConfig& s = cfg_.servo[ref.index];
    float peak = peakDps(s);
    if (s.maxDps > 0 && peak > s.maxDps) {
        out.line(fmt("[MSG:WARN: servo %.0f would need %.0f deg/s, max_dps is %.0f]", ref.index, peak, s.maxDps));
    }
    uint32_t period = cfg_.scene.periodMs;
    if (s.mode == Mode::Return && period > 0 && servoCycleMs(s) > period) {
        out.line(fmt("[MSG:WARN: servo %.0f needs %.0f ms but the scene period is %.0f ms]", ref.index,
                     servoCycleMs(s), period));
    }
}

// A return-mode servo whose delay + move + dwell + move exceeds a fixed scene period is cut
// off at every wrap and jumps back to its start.
void Parser::warnShortPeriod(ReplySink& out) {
    const uint32_t period = cfg_.scene.periodMs;
    if (period == 0) return;
    std::string list;
    for (int i = 0; i < kMaxServos; ++i) {
        const ServoConfig& s = cfg_.servo[i];
        if (servoActive(s) && s.mode == Mode::Return && servoCycleMs(s) > period) {
            list += (list.empty() ? "" : ",") + std::to_string(i);
        }
    }
    if (!list.empty()) out.line("[MSG:WARN: scene period is shorter than the cycle of servos " + list + "]");
}

bool Parser::restore(const std::string& text, ReplySink& out) {
    // Build the restored configuration separately - from the defaults, since a backup lists
    // only non-default settings - and apply it only if every line is a valid setting, so a
    // wrong or empty file leaves the live configuration untouched.
    auto next = std::make_unique<Config>();
    setDefaults(*next);
    Settings target(*next);
    int applied = 0, failed = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty()) continue;
        std::string err;
        const size_t eq = line.find('=');
        SettingRef ref;
        if (line[0] != '$' || eq == std::string::npos) {
            err = "not a $setting=value line";
        } else if (!target.find(trim(line.substr(1, eq - 1)), ref)) {
            err = "unknown setting";
        } else {
            target.set(ref, trim(line.substr(eq + 1)), err);
        }
        if (err.empty()) {
            ++applied;
        } else {
            ++failed;
            out.line("[MSG:ERR: " + line + ": " + err + "]");
        }
    }
    if (failed > 0) {
        out.line("[MSG:ERR: Restore rejected, nothing was changed]");
        return false;
    }
    if (applied == 0) {
        out.line("[MSG:ERR: The file contains no settings, nothing was changed (use Factory defaults to reset)]");
        return false;
    }

    Guard g(host_);
    // Restart-only settings take effect at boot, so compare with what the device booted with.
    const Config& booted = bootCfg_ ? *bootCfg_ : cfg_;
    bool needsRestart = false;
    target.forEachAll([&](const SettingRef& r) {
        if ((r.field->flags & kReboot) && !target.equals(r, booted)) needsRestart = true;
    });
    cfg_ = *next;
    scene_.rejoin(-1);
    out.line("[MSG:INFO: Config restored (" + std::to_string(applied) + " settings)]");
    if (needsRestart) out.line("[MSG:INFO: Restart required to apply the restored config]");
    warnShortPeriod(out);
    return true;
}

void Parser::markBooted() {
    Guard g(host_);
    bootCfg_ = std::make_unique<Config>(cfg_);
}

Err Parser::servoArg(const std::string& value, int& servo, bool allowAll, std::string& msg) const {
    if (allowAll && value == "*") {
        servo = -1;
        return Err::Ok;
    }
    char* end = nullptr;
    long n = std::strtol(value.c_str(), &end, 10);
    if (value.empty() || *end) {
        msg = allowAll ? "expected a servo number or *" : "expected a servo number";
        return Err::BadValue;
    }
    if (n < 0 || n >= kMaxServos || !servoActive(cfg_.servo[n])) {
        msg = "servo " + value + " is not enabled";
        return Err::NoSuchServo;
    }
    servo = static_cast<int>(n);
    return Err::Ok;
}

Err Parser::command(const std::string& name, const std::string& value, bool hasValue, ReplySink& out,
                    std::string& msg) {
    auto is = [&](const char* a, const char* b = nullptr, const char* c = nullptr) {
        return ieq(name, a) || (b && ieq(name, b)) || (c && ieq(name, c));
    };

    if (is("I", "Build/Info")) {
        host_.info(out);
        return Err::Ok;
    }
    if (is("S", "Settings/List")) {
        listSettings(value, out);
        return Err::Ok;
    }
    if (is("CD", "Config/Dump") && !hasValue) {
        Guard g(host_);
        std::string yaml = settings_.dumpYaml();
        size_t pos = 0;
        while (pos < yaml.size()) {
            size_t nl = yaml.find('\n', pos);
            out.line(yaml.substr(pos, nl - pos));
            pos = nl + 1;
        }
        return Err::Ok;
    }
    if (is("CS", "Config/Save", "Save") || is("CD", "Config/Dump")) {
        std::string text;
        {
            Guard g(host_);
            text = settings_.dumpChanged();
        }
        if (!host_.save(text, msg)) return Err::FlashFailed;
        out.line("[MSG:INFO: Config saved (" + std::to_string(text.size()) + " bytes)]");
        return Err::Ok;
    }
    if (is("RST")) {
        if (value != "*" && value != "$") {
            msg = "use $RST=* to restore all defaults";
            return Err::BadValue;
        }
        {
            Guard g(host_);
            setDefaults(cfg_);
            scene_.rejoin(-1);
        }
        if (!host_.erase(msg)) return Err::FlashFailed;
        out.line("[MSG:INFO: Defaults restored, restart to apply pin changes]");
        return Err::Ok;
    }
    if (is("Bye", "System/Restart")) {
        host_.reboot();
        return Err::Ok;
    }
    if (is("Status")) {
        Guard g(host_);
        out.line(scene_.status());
        return Err::Ok;
    }
    if (is("Positions", "Servo/Positions")) {
        Guard g(host_);
        out.line(scene_.positions());
        return Err::Ok;
    }
    if (is("Scene/Stop")) return host_.setRunning(false, "command"), Err::Ok;
    if (is("Scene/Go")) return host_.setRunning(true, "command"), Err::Ok;
    if (is("Scene/Reset")) {
        realtime(0x18, out);
        return Err::Ok;
    }
    if (is("Servo/Stop", "Servo/Go")) {
        Guard g(host_);
        int servo;
        Err e = servoArg(value, servo, true, msg);
        if (e != Err::Ok) return e;
        scene_.setServoRun(servo, is("Servo/Go"));
        return Err::Ok;
    }
    if (is("Servo/Test")) {
        Guard g(host_);
        size_t comma = value.find(',');
        int servo;
        Err e = servoArg(trim(value.substr(0, comma)), servo, false, msg);
        if (e != Err::Ok) return e;
        char* end = nullptr;
        std::string degStr = comma == std::string::npos ? "" : trim(value.substr(comma + 1));
        double deg = std::strtod(degStr.c_str(), &end);
        if (degStr.empty() || *end || !std::isfinite(deg)) {
            msg = "use $Servo/Test=<servo>,<degrees>";
            return Err::BadValue;
        }
        float range = cfg_.servo[servo].rangeDeg;
        if (deg < 0 || deg > range) {
            msg = "angle must be within 0.." + std::to_string(static_cast<int>(range));
            return Err::OutOfRange;
        }
        scene_.test(servo, static_cast<float>(deg));
        return Err::Ok;
    }
    if (is("Servo/Release")) {
        Guard g(host_);
        int servo;
        Err e = servoArg(value, servo, true, msg);
        if (e != Err::Ok) return e;
        scene_.release(servo);
        return Err::Ok;
    }
    return host_.command(name, value, hasValue, out, msg);
}

void Parser::listSettings(const std::string& prefix, ReplySink& out) {
    Guard g(host_);
    settings_.forEach(prefix, [&](const SettingRef& r) {
        out.line("$" + settings_.path(r) + "=" + settings_.get(r, true));
    });
}

void Parser::help(ReplySink& out) {
    static const char* const kLines[] = {
        "[HLP:$$ $S $I $CD $CS $RST=* $Bye $Status $Positions]",
        "[HLP:$Scene/Stop $Scene/Go $Scene/Reset   realtime: ! ~ ? Ctrl-X]",
        "[HLP:$Servo/Stop=n|* $Servo/Go=n|* $Servo/Test=n,deg $Servo/Release=n|*]",
        "[HLP:$/servo/n/<field>[=value]  $/scene/... $/pwm/... $/i2c/... $/pca/n/... $/button/...]",
        "[HLP:$AP/SSID $AP/Password $AP/Channel $AP/IP $AP/Captive $Hostname]",
        "[HLP:$Firmware/Update[=url]  (default http://<first client>:9999/firmware.bin)]",
    };
    for (const char* l : kLines) out.line(l);
}

}  // namespace ss
