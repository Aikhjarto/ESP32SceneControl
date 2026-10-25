#pragma once

namespace ss::app {

// Starts the periodic motion task (scene tick, Stop/Go button, output writes).
// Call after outputs().begin().
void startMotion();

// Global Stop/Go, used by the button, the realtime commands and the web UI.
void setRunning(bool run, const char* source);

}  // namespace ss::app
