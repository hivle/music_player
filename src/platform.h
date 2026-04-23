#pragma once
#include <string>
#include <vector>

namespace platform {

// Show a native folder picker. Returns the selected absolute path, or an
// empty string if the user cancelled or no picker is available.
std::string pick_folder(const std::string& initial_dir = "");

// Candidate absolute paths to a Latin-friendly UI font (DejaVu, Segoe UI,
// Helvetica, ...) in order of preference. First entry is the best guess.
std::vector<std::string> find_ui_fonts();

// Candidate absolute paths to a font with broad CJK (Chinese / Japanese /
// Korean) coverage, in order of preference. Empty if none are installed.
std::vector<std::string> find_cjk_fonts();

} // namespace platform
