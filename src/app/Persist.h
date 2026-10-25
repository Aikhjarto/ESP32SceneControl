// The saved configuration is the text of "$/path=value" lines (non-default settings only),
// stored as one NVS blob and replayed through the command parser at boot.
#pragma once

#include <string>

namespace ss::app::persist {

bool load(std::string& text);
bool save(const std::string& text, std::string& err);
bool erase(std::string& err);

}  // namespace ss::app::persist
