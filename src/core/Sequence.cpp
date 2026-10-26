#include "core/Sequence.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace ss {

static bool ieqn(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (!b[i] || std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
    }
    return b[n] == 0;
}

bool parseSequence(const char* text, Sequence& seq, std::string& err) {
    seq.count = 0;
    const char* p = text;
    while (*p) {
        while (*p == ' ' || *p == ',') ++p;
        if (!*p) break;
        if (seq.count == kMaxSwitchPoints) {
            err = "at most " + std::to_string(kMaxSwitchPoints) + " switch points";
            return false;
        }
        char* end = nullptr;
        if (!std::isdigit(static_cast<unsigned char>(*p))) {
            err = "expected ms:on or ms:off, e.g. 0:on,500:off";
            return false;
        }
        unsigned long t = std::strtoul(p, &end, 10);
        while (*end == ' ') ++end;
        if (*end != ':' || t > 3600000) {
            err = t > 3600000 ? "switch time must be at most 3600000 ms" : "expected ms:on or ms:off, e.g. 0:on,500:off";
            return false;
        }
        p = end + 1;
        while (*p == ' ') ++p;
        size_t n = 0;
        while (p[n] && p[n] != ',' && p[n] != ' ') ++n;
        bool on;
        if (ieqn(p, "on", n) || ieqn(p, "1", n) || ieqn(p, "high", n)) {
            on = true;
        } else if (ieqn(p, "off", n) || ieqn(p, "0", n) || ieqn(p, "low", n)) {
            on = false;
        } else {
            err = "state must be on or off";
            return false;
        }
        if (seq.count > 0 && t <= seq.timeMs[seq.count - 1]) {
            err = "switch times must increase";
            return false;
        }
        seq.timeMs[seq.count] = static_cast<uint32_t>(t);
        seq.on[seq.count] = on;
        ++seq.count;
        p += n;
    }
    return true;
}

bool sequenceState(const Sequence& seq, uint32_t tMs) {
    if (seq.count == 0) return false;
    bool state = seq.on[seq.count - 1];  // wraps around from the end of the previous cycle
    for (int k = 0; k < seq.count && seq.timeMs[k] <= tMs; ++k) state = seq.on[k];
    return state;
}

}  // namespace ss
