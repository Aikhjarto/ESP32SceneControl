#pragma once

#include <string>

#include "core/Config.h"

namespace ss::net {

// Starts the soft access point. When captive is set, DHCP hands out the ESP as DNS server.
void startAccessPoint(const NetConfig& cfg);

const std::string& apSsid();
const std::string& apIp();

}  // namespace ss::net
