#pragma once

#include <string>

namespace ss::net {

// Minimal DNS server answering every A query with the AP's address, so phones
// detect a captive portal and open the web UI automatically.
void startCaptiveDns(const std::string& ip);

}  // namespace ss::net
