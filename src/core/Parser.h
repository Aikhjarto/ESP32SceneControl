// FluidNC-style command parser shared by the serial port, WebSocket clients and the
// boot-time config replay. Every line is answered with "ok" or "error:N" (optionally
// preceded by output lines); realtime characters (! ~ ? Ctrl-X) are handled separately.
#pragma once

#include <string>

#include "core/Config.h"
#include "core/Scene.h"
#include "core/Settings.h"

namespace ss {

class ReplySink {
public:
    virtual ~ReplySink() = default;
    virtual void line(const std::string& text) = 0;
};

// Firmware services the parser needs; the native tests provide a fake.
class Host {
public:
    virtual ~Host() = default;
    virtual void lock() {}
    virtual void unlock() {}
    virtual void setRunning(bool run, const char* source) = 0;
    virtual bool save(const std::string& text, std::string& err) = 0;
    virtual bool erase(std::string& err) = 0;
    virtual void reboot() = 0;
    virtual void info(ReplySink& out) = 0;
};

class Parser {
public:
    Parser(Config& cfg, Scene& scene, Host& host) : cfg_(cfg), scene_(scene), host_(host), settings_(cfg) {}

    void execute(const std::string& line, ReplySink& out);

    static bool isRealtime(char c) { return c == '!' || c == '~' || c == '?' || c == 0x18; }
    void realtime(char c, ReplySink& out);

    // Runs saved "$/path=value" lines at boot. Only failures are reported.
    void replay(const std::string& text, ReplySink& out);

    Settings& settings() { return settings_; }

private:
    struct Guard {
        explicit Guard(Host& h) : h(h) { h.lock(); }
        ~Guard() { h.unlock(); }
        Host& h;
    };

    Err run(const std::string& line, ReplySink& out, std::string& msg);
    Err command(const std::string& name, const std::string& value, bool hasValue, ReplySink& out, std::string& msg);
    Err setting(const std::string& path, const std::string& value, bool hasValue, ReplySink& out, std::string& msg);
    Err servoArg(const std::string& value, int& servo, bool allowAll, std::string& msg) const;
    void afterChange(const SettingRef& ref, ReplySink& out);
    void listSettings(const std::string& prefix, ReplySink& out);
    void help(ReplySink& out);

    Config& cfg_;
    Scene& scene_;
    Host& host_;
    Settings settings_;
    bool replaying_ = false;
};

}  // namespace ss
