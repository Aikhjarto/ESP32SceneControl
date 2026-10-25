#pragma once

#include <string>

#include "core/Config.h"

namespace ss::net {

// Starts the soft access point. When captive is set, DHCP hands out the ESP as DNS server.
void startAccessPoint(const NetConfig& cfg);

const std::string& apSsid();
const std::string& apIp();

// Address of the first station that got a DHCP lease since boot (empty if none yet).
std::string firstClientIp();

}  // namespace ss::net
