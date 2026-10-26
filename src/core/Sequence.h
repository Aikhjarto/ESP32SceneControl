// Switch-point sequences for digital outputs: "ms:on|off,..." within the scene cycle,
// e.g. "0:on,500:off,1200:on,1500:off". Times strictly increasing, at most kMaxSwitchPoints.
#pragma once

#include <cstdint>
#include <string>

#include "core/Config.h"

namespace ss {

struct Sequence {
    int count = 0;
    uint32_t timeMs[kMaxSwitchPoints] = {};
    bool on[kMaxSwitchPoints] = {};
};

// Returns false with a message if the text is not a valid sequence (empty text is valid).
bool parseSequence(const char* text, Sequence& seq, std::string& err);

// State at time tMs within the cycle: that of the last point at or before tMs; before the
// first point, that of the last point (the sequence wraps with the cycle). Empty: off.
bool sequenceState(const Sequence& seq, uint32_t tMs);

}  // namespace ss
