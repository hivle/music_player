#pragma once
#include <string>
#include <vector>

namespace platform {

// Show a native folder picker. Returns the selected absolute path, or an
// empty string if the user cancelled or no picker is available.
std::string pick_folder(const std::string& initial_dir = "");

// Return candidate absolute paths to TTF/OTF/TTC font files on the current
// system that have broad Unicode coverage (Latin + CJK + Cyrillic + ...).
// First element is the best guess. Empty if nothing was found.
std::vector<std::string> find_ui_fonts();

} // namespace platform
