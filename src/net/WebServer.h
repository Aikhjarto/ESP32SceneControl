#pragma once

#include <string>

namespace ss::net {

// Serves the embedded web UI, /ws (same command protocol as serial) and /config.txt
// (backup/restore). With captive set, unknown URLs redirect to the UI.
void startWebServer(const std::string& ip, bool captive);

int wsClientCount();

}  // namespace ss::net
