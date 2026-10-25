#include "core/LineReader.h"

namespace ss {

LineReader::Event LineReader::feed(char c) {
    const bool afterCr = lastWasCr_;
    lastWasCr_ = c == '\r';

    if (c == 0x18) {
        realtime_ = c;
        return Event::Realtime;
    }
    if ((c == '!' || c == '~' || c == '?') && buf_.find('=') == std::string::npos) {
        realtime_ = c;
        return Event::Realtime;
    }
    if (c == '\r' || c == '\n') {
        if (c == '\n' && afterCr) return Event::None;  // CR LF counts as one line end
        line_.swap(buf_);
        buf_.clear();
        return Event::Line;
    }
    if (c == '\b' || c == 0x7F) {
        if (!buf_.empty()) buf_.pop_back();
    } else if (buf_.size() < maxLine_) {
        buf_ += c;
    }
    return Event::None;
}

}  // namespace ss
