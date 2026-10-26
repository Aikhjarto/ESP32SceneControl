#pragma once

#include <cstdint>

namespace ss::app {

// Starts the periodic motion task (scene tick, Stop/Go button, output writes).
// Call after outputs().begin().
void startMotion();

// Global Stop/Go, used by the button, the realtime commands and the web UI.
void setRunning(bool run, const char* source);

// Increments on every Stop/Go change; lets a caller tell whether anyone changed it since.
uint32_t runGeneration();

}  // namespace ss::app
