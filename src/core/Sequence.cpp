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

struct PatternDesc {
    const char* name;
    OutState state;
    int count;  // number of parameters
    uint16_t def[kMaxPatternParams];
    uint16_t min[kMaxPatternParams];
    uint16_t max[kMaxPatternParams];
    const char* usage;
};

static const PatternDesc kPatterns[] = {
    {"blink", OutState::Blink, 2, {500, 500}, {1, 1}, {60000, 60000}, "blink on_ms/off_ms"},
    {"flash", OutState::Flash, 4, {2, 60, 120, 800}, {1, 1, 1, 0}, {20, 60000, 60000, 60000},
     "flash count/on_ms/gap_ms/pause_ms"},
    {"flicker", OutState::Flicker, 2, {75, 50}, {0, 1}, {100, 60000}, "flicker on_pct/slot_ms"},
    {"heartbeat", OutState::Heartbeat, 1, {1000}, {100}, {60000}, "heartbeat period_ms"},
};

static const char* const kExpected = "expected ms:state, e.g. 0:on,500:off,1000:blink 100/200";

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
            err = kExpected;
            return false;
        }
        unsigned long t = std::strtoul(p, &end, 10);
        while (*end == ' ') ++end;
        if (*end != ':' || t > 3600000) {
            err = t > 3600000 ? "switch time must be at most 3600000 ms" : kExpected;
            return false;
        }
        p = end + 1;
        while (*p == ' ') ++p;
        size_t n = 0;
        while (p[n] && p[n] != ',' && p[n] != ' ' && p[n] != '/') ++n;
        const int k = seq.count;
        uint16_t* par = seq.param[k];
        for (int i = 0; i < kMaxPatternParams; ++i) par[i] = 0;
        const PatternDesc* pat = nullptr;
        if (ieqn(p, "on", n) || ieqn(p, "1", n) || ieqn(p, "high", n)) {
            seq.state[k] = OutState::On;
        } else if (ieqn(p, "off", n) || ieqn(p, "0", n) || ieqn(p, "low", n)) {
            seq.state[k] = OutState::Off;
        } else {
            for (const PatternDesc& d : kPatterns) {
                if (ieqn(p, d.name, n)) pat = &d;
            }
            if (!pat) {
                err = "state must be on, off, blink, flash, flicker or heartbeat";
                return false;
            }
            seq.state[k] = pat->state;
            for (int i = 0; i < pat->count; ++i) par[i] = pat->def[i];
        }
        p += n;
        // Parameters: "blink 100/200" or "blink/100/200". A number followed by ':' is the
        // next switch point instead ("0:blink 500:off").
        const char* q = p;
        while (*q == ' ') ++q;
        if (*q == '/') ++q;
        const char* r = q;
        while (std::isdigit(static_cast<unsigned char>(*r))) ++r;
        while (*r == ' ') ++r;
        const bool hasParams = std::isdigit(static_cast<unsigned char>(*q)) && *r != ':';
        if (hasParams) {
            if (!pat) {
                err = "on and off take no parameters";
                return false;
            }
            int i = 0;
            p = q;
            for (;;) {
                if (i == pat->count || !std::isdigit(static_cast<unsigned char>(*p))) {
                    err = std::string("expected ") + pat->usage + " (any trailing values may be left out)";
                    return false;
                }
                unsigned long v = std::strtoul(p, &end, 10);
                if (v < pat->min[i] || v > pat->max[i]) {
                    err = std::string(pat->usage) + ": value " + std::to_string(i + 1) + " must be " +
                          std::to_string(pat->min[i]) + ".." + std::to_string(pat->max[i]);
                    return false;
                }
                par[i++] = static_cast<uint16_t>(v);
                p = end;
                if (*p != '/') break;
                ++p;
            }
        }
        if (*p && *p != ',' && *p != ' ') {
            err = kExpected;
            return false;
        }
        if (seq.count > 0 && t <= seq.timeMs[seq.count - 1]) {
            err = "switch times must increase";
            return false;
        }
        seq.timeMs[k] = static_cast<uint32_t>(t);
        ++seq.count;
    }
    return true;
}

// Integer hash for the flicker; web/app.js uses the same one (Math.imul) so the UI preview
// matches the output exactly.
static uint32_t flickerHash(uint32_t seed, uint32_t slot) {
    uint32_t h = seed * 0x9E3779B1u + slot * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

// State of a pattern ms after its switch point.
static bool patternOn(OutState st, const uint16_t* p, uint32_t ms, uint32_t seed) {
    switch (st) {
        case OutState::Off:
            return false;
        case OutState::On:
            return true;
        case OutState::Blink:
            return ms % (p[0] + p[1]) < p[0];
        case OutState::Flash: {
            const uint32_t step = p[1] + p[2];  // one flash and the gap after it
            const uint32_t cycle = p[0] * step - p[2] + p[3];
            const uint32_t e = cycle ? ms % cycle : 0;
            return e / step < p[0] && e % step < p[1];
        }
        case OutState::Flicker:
            return flickerHash(seed, ms / p[1]) % 100 < p[0];
        case OutState::Heartbeat: {
            const uint32_t e = ms % p[0] * 100;  // in 1/100 of the period
            return e < 10u * p[0] || (e >= 25u * p[0] && e < 35u * p[0]);
        }
    }
    return false;
}

bool sequenceState(const Sequence& seq, uint32_t tMs, uint32_t periodMs, uint32_t seed) {
    // Points at or after the end of the cycle are never reached, so they don't count.
    int reached = seq.count;
    while (periodMs > 0 && reached > 0 && seq.timeMs[reached - 1] >= periodMs) --reached;
    if (reached == 0) return false;
    int k = -1;
    for (int i = 0; i < reached && seq.timeMs[i] <= tMs; ++i) k = i;
    uint32_t ms;
    if (k >= 0) {
        ms = tMs - seq.timeMs[k];
    } else {  // wraps around from the end of the previous cycle
        k = reached - 1;
        ms = periodMs > seq.timeMs[k] ? tMs + (periodMs - seq.timeMs[k]) : tMs;
    }
    return patternOn(seq.state[k], seq.param[k], ms, seed * 16 + static_cast<uint32_t>(k));
}

uint32_t shortestPatternMs(const Sequence& seq) {
    uint32_t shortest = 0;
    auto take = [&](uint32_t v) {
        if (v > 0 && (shortest == 0 || v < shortest)) shortest = v;
    };
    for (int k = 0; k < seq.count; ++k) {
        const uint16_t* p = seq.param[k];
        switch (seq.state[k]) {
            case OutState::Blink: take(p[0]), take(p[1]); break;
            case OutState::Flash: take(p[1]), take(p[2]), take(p[3]); break;
            case OutState::Flicker: take(p[1]); break;
            case OutState::Heartbeat: take(p[0] / 10); break;
            default: break;
        }
    }
    return shortest;
}

}  // namespace ss
