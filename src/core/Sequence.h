// Switch-point sequences for digital outputs: "ms:state,..." within the scene cycle, e.g.
// "0:on,500:off,1200:blink 100/200,4000:off". Times strictly increasing, at most
// kMaxSwitchPoints. A state is on, off or a blinking pattern with optional parameters
// (separated by '/', missing ones take the defaults):
//   blink [on_ms/off_ms]                  500/500   regular on/off
//   flash [count/on_ms/gap_ms/pause_ms]   2/60/120/800   groups of short flashes
//   flicker [on_pct/slot_ms]              75/50     random on/off per slot (candle, fire)
//   heartbeat [period_ms]                 1000      two beats (10 % on each), then a pause
// A pattern starts at its switch point, so it stays in sync with the scene clock.
// The web UI mirrors parsing and evaluation (parseSeq / seqState in web/app.js).
#pragma once

#include <cstdint>
#include <string>

#include "core/Config.h"

namespace ss {

enum class OutState : uint8_t { Off, On, Blink, Flash, Flicker, Heartbeat };
constexpr int kMaxPatternParams = 4;

struct Sequence {
    int count = 0;
    uint32_t timeMs[kMaxSwitchPoints] = {};
    OutState state[kMaxSwitchPoints] = {};
    uint16_t param[kMaxSwitchPoints][kMaxPatternParams] = {};  // defaults filled in
};

// Returns false with a message if the text is not a valid sequence (empty text is valid).
bool parseSequence(const char* text, Sequence& seq, std::string& err);

// State at time tMs within a cycle of periodMs (0: unlimited): that of the last point at or
// before tMs; before the first point, that of the last point within the cycle (the sequence
// wraps with it). Points at or after periodMs are never reached. No point reached: off.
// seed varies the random flicker between outputs (the output index).
bool sequenceState(const Sequence& seq, uint32_t tMs, uint32_t periodMs = 0, uint32_t seed = 0);

// Shortest on or off time of the patterns in seq in ms (0 if it has none): times shorter
// than a motion tick are not shown faithfully.
uint32_t shortestPatternMs(const Sequence& seq);

}  // namespace ss
