#pragma once
#include "imgui.h"

namespace theme {

// Luna palette
constexpr ImU32 LunaTitleTop    = IM_COL32(0x2F, 0x71, 0xCD, 0xFF);
constexpr ImU32 LunaTitleMid    = IM_COL32(0x1C, 0x5BC0, 0xFF, 0xFF); // unused safety
constexpr ImU32 LunaTitleBot    = IM_COL32(0x08, 0x40, 0xA0, 0xFF);
constexpr ImU32 LunaTitleGloss  = IM_COL32(0x7E, 0xB3, 0xEE, 0xFF);
constexpr ImU32 LunaBody        = IM_COL32(0xEC, 0xE9, 0xD8, 0xFF);
constexpr ImU32 LunaBodyDark    = IM_COL32(0xD4, 0xD0, 0xC8, 0xFF);
constexpr ImU32 LunaBorder      = IM_COL32(0x0A, 0x24, 0x6A, 0xFF);
constexpr ImU32 LunaInset       = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
constexpr ImU32 LunaSelection   = IM_COL32(0x31, 0x6A, 0xC5, 0xFF);

void apply_style();

// Draw a Luna-style blue gradient titlebar with rounded top corners.
void draw_titlebar(ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, const char* title);

// Glossy silver XP button. Returns true when clicked. Size in pixels.
bool xp_button(const char* label, ImVec2 size = ImVec2(0, 0));

// Glossy green XP "Start" style button.
bool xp_green_button(const char* label, ImVec2 size = ImVec2(0, 0));

// XP sunken panel (used for list/art frames).
void draw_sunken_panel(ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, ImU32 fill);

// Blue glossy progress bar. value in [0,1]. Click/drag returns new value through out_value.
// Returns true if the user scrubbed.
bool xp_progress(const char* id, float value, ImVec2 size, float* out_value);

// Vertical gradient fill helper.
void vgradient(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bot);

} // namespace theme
