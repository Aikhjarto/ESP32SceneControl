#pragma once

#include <cstddef>

#include "esp_http_server.h"

namespace ss::net {

// Receives exactly len bytes of a request body. Gives up after maxTimeouts consecutive
// receive timeouts (5 s each by default), so a client that vanishes mid-upload cannot
// block the single web server task forever.
inline bool recvExact(httpd_req_t* req, char* buf, size_t len, int maxTimeouts = 3) {
    size_t have = 0;
    int timeouts = 0;
    while (have < len) {
        int r = httpd_req_recv(req, buf + have, len - have);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts >= maxTimeouts) return false;
            continue;
        }
        if (r <= 0) return false;
        timeouts = 0;
        have += r;
    }
    return true;
}

}  // namespace ss::net
