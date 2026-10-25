// Shared application state. Config and Scene are guarded by one recursive lock that the
// motion task holds once per tick and the command executor holds per command.
#pragma once

#include <string>

#include "core/Config.h"
#include "core/Scene.h"
#include "out/Outputs.h"

namespace ss::app {

Config& config();
Scene& scene();
Outputs& outputs();

void initLock();
void lock();
void unlock();

struct Locked {
    Locked() { lock(); }
    ~Locked() { unlock(); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
};

// Asynchronous message to every connected channel (serial and WebSocket), e.g. "[MSG:INFO: Hold (button)]".
using Broadcaster = void (*)(const std::string& text);
void setBroadcaster(Broadcaster b);
void broadcast(const std::string& text);

// Problems found while bringing up outputs, shown by $I.
std::string& bootLog();

}  // namespace ss::app
