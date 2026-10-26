// Assembles a serial byte stream into command lines. Realtime characters (! ~ ? Ctrl-X)
// act immediately, except ! ~ ? inside the value of a "$name=value" line, where they are
// ordinary text (so passwords and servo names may contain them). Only a line starting with
// '$' has a value, so stray input without '$' never disables the serial stop.
#pragma once

#include <cstddef>
#include <string>

namespace ss {

class LineReader {
public:
    enum class Event { None, Realtime, Line };

    explicit LineReader(size_t maxLine = 256) : maxLine_(maxLine) {}

    // After Event::Line, line() holds the completed line; after Event::Realtime, realtimeChar().
    Event feed(char c);
    const std::string& line() const { return line_; }
    char realtimeChar() const { return realtime_; }

private:
    size_t maxLine_;
    std::string buf_;
    std::string line_;
    char realtime_ = 0;
    bool lastWasCr_ = false;
};

}  // namespace ss
