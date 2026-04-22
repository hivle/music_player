#include "theme.h"
#include "imgui_internal.h"

#include <algorithm>

namespace theme {

void apply_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.f;
    s.ChildRounding = 2.f;
    s.FrameRounding = 3.f;
    s.GrabRounding = 3.f;
    s.PopupRounding = 2.f;
    s.ScrollbarRounding = 0.f;
    s.WindowPadding = ImVec2(6, 6);
    s.FramePadding = ImVec2(6, 3);
    s.ItemSpacing = ImVec2(6, 5);
    s.ItemInnerSpacing = ImVec2(4, 3);
    s.ScrollbarSize = 16.f;

    ImVec4* c = s.Colors;
    auto col = [](int r, int g, int b, int a = 255) {
        return ImVec4(r / 255.f, g / 255.f, b / 255.f, a / 255.f);
    };
    c[ImGuiCol_WindowBg]         = col(0xEC, 0xE9, 0xD8);
    c[ImGuiCol_ChildBg]          = col(0xEC, 0xE9, 0xD8);
    c[ImGuiCol_PopupBg]          = col(0xFF, 0xFF, 0xFF);
    c[ImGuiCol_Border]           = col(0x0A, 0x24, 0x6A);
    c[ImGuiCol_BorderShadow]     = col(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]          = col(0xFF, 0xFF, 0xFF);
    c[ImGuiCol_FrameBgHovered]   = col(0xF4, 0xF4, 0xFF);
    c[ImGuiCol_FrameBgActive]    = col(0xDC, 0xE4, 0xF4);
    c[ImGuiCol_TitleBg]          = col(0x08, 0x40, 0xA0);
    c[ImGuiCol_TitleBgActive]    = col(0x08, 0x40, 0xA0);
    c[ImGuiCol_Text]             = col(0x00, 0x00, 0x00);
    c[ImGuiCol_TextDisabled]     = col(0x80, 0x80, 0x80);
    c[ImGuiCol_Button]           = col(0xED, 0xED, 0xE5);
    c[ImGuiCol_ButtonHovered]    = col(0xF5, 0xF5, 0xEC);
    c[ImGuiCol_ButtonActive]     = col(0xC9, 0xD5, 0xE9);
    c[ImGuiCol_Header]           = col(0x31, 0x6A, 0xC5);
    c[ImGuiCol_HeaderHovered]    = col(0x5A, 0x8B, 0xD4);
    c[ImGuiCol_HeaderActive]     = col(0x31, 0x6A, 0xC5);
    c[ImGuiCol_ScrollbarBg]      = col(0xD4, 0xD0, 0xC8);
    c[ImGuiCol_ScrollbarGrab]    = col(0xEC, 0xE9, 0xD8);
    c[ImGuiCol_ScrollbarGrabHovered] = col(0xDC, 0xDC, 0xCC);
    c[ImGuiCol_ScrollbarGrabActive]  = col(0xB0, 0xB0, 0xA0);
    c[ImGuiCol_Separator]        = col(0xAC, 0xA8, 0x99);
    c[ImGuiCol_SliderGrab]       = col(0xEC, 0xE9, 0xD8);
    c[ImGuiCol_SliderGrabActive] = col(0xC9, 0xD5, 0xE9);
    c[ImGuiCol_ResizeGrip]       = col(0, 0, 0, 0);
}

void vgradient(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bot) {
    dl->AddRectFilledMultiColor(a, b, top, top, bot, bot);
}

void draw_titlebar(ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, const char* title) {
    // Base gradient (top to bottom: light blue -> deeper blue)
    ImU32 top   = IM_COL32(0x2C, 0x6D, 0xCF, 0xFF);
    ImU32 mid   = IM_COL32(0x15, 0x51, 0xBB, 0xFF);
    ImU32 bot   = IM_COL32(0x08, 0x2B, 0x88, 0xFF);
    float h = p_max.y - p_min.y;
    ImVec2 midp(p_min.x, p_min.y + h * 0.55f);
    dl->AddRectFilledMultiColor(p_min, ImVec2(p_max.x, midp.y), top, top, mid, mid);
    dl->AddRectFilledMultiColor(ImVec2(p_min.x, midp.y), p_max, mid, mid, bot, bot);
    // Highlight stripe at very top (gloss)
    dl->AddRectFilledMultiColor(
        p_min, ImVec2(p_max.x, p_min.y + 3),
        IM_COL32(0xAE, 0xCB, 0xF1, 0xFF), IM_COL32(0xAE, 0xCB, 0xF1, 0xFF),
        IM_COL32(0x6D, 0xA0, 0xE2, 0x00), IM_COL32(0x6D, 0xA0, 0xE2, 0x00));
    // Thin dark line at bottom
    dl->AddRectFilled(ImVec2(p_min.x, p_max.y - 1), p_max, IM_COL32(0x00, 0x1D, 0x67, 0xFF));
    // Title text with shadow
    ImFont* font = ImGui::GetFont();
    const char* t = title ? title : "";
    ImVec2 ts = ImGui::CalcTextSize(t);
    ImVec2 tp(p_min.x + 10, p_min.y + (h - ts.y) * 0.5f);
    dl->AddText(font, font->FontSize, ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 160), t);
    dl->AddText(font, font->FontSize, tp, IM_COL32(255, 255, 255, 255), t);
}

void draw_sunken_panel(ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, ImU32 fill) {
    dl->AddRectFilled(p_min, p_max, fill);
    // Inset shadow (top+left dark, bottom+right light)
    ImU32 dark = IM_COL32(0x80, 0x80, 0x80, 0xFF);
    ImU32 shade = IM_COL32(0xAC, 0xA8, 0x99, 0xFF);
    ImU32 light = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
    dl->AddLine(ImVec2(p_min.x, p_min.y), ImVec2(p_max.x - 1, p_min.y), dark);
    dl->AddLine(ImVec2(p_min.x, p_min.y), ImVec2(p_min.x, p_max.y - 1), dark);
    dl->AddLine(ImVec2(p_min.x + 1, p_min.y + 1), ImVec2(p_max.x - 2, p_min.y + 1), shade);
    dl->AddLine(ImVec2(p_min.x + 1, p_min.y + 1), ImVec2(p_min.x + 1, p_max.y - 2), shade);
    dl->AddLine(ImVec2(p_min.x, p_max.y - 1), ImVec2(p_max.x - 1, p_max.y - 1), light);
    dl->AddLine(ImVec2(p_max.x - 1, p_min.y), ImVec2(p_max.x - 1, p_max.y - 1), light);
}

static bool button_impl(const char* label, ImVec2 size,
                        ImU32 base_top, ImU32 base_bot,
                        ImU32 hov_top,  ImU32 hov_bot,
                        ImU32 act_top,  ImU32 act_bot,
                        ImU32 text_col, ImU32 border) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return false;
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGuiID id = win->GetID(label);
    ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    ImVec2 pos = win->DC.CursorPos;
    ImVec2 sz = ImGui::CalcItemSize(size, label_size.x + style.FramePadding.x * 2, label_size.y + style.FramePadding.y * 2);
    ImRect bb(pos, ImVec2(pos.x + sz.x, pos.y + sz.y));
    ImGui::ItemSize(sz, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

    ImU32 top = base_top, bot = base_bot;
    if (held && hovered) { top = act_top; bot = act_bot; }
    else if (hovered)    { top = hov_top; bot = hov_bot; }

    ImDrawList* dl = win->DrawList;
    float r = style.FrameRounding;
    dl->AddRectFilled(bb.Min, bb.Max, bot, r);
    // Gradient fill (clipped-ish by drawing a filled rect, then over it a shorter gradient top half)
    ImVec2 half(bb.Max.x, bb.Min.y + (bb.Max.y - bb.Min.y) * 0.55f);
    dl->AddRectFilledMultiColor(bb.Min, half, top, top, bot, bot);
    // Top gloss highlight (white fading to transparent over top ~40%)
    ImVec2 gloss_max(bb.Max.x, bb.Min.y + (bb.Max.y - bb.Min.y) * 0.45f);
    dl->AddRectFilledMultiColor(
        bb.Min, gloss_max,
        IM_COL32(255, 255, 255, 130), IM_COL32(255, 255, 255, 130),
        IM_COL32(255, 255, 255, 0),   IM_COL32(255, 255, 255, 0));
    // Border
    dl->AddRect(bb.Min, bb.Max, border, r, 0, 1.0f);
    // Text
    ImVec2 tpos(bb.Min.x + (sz.x - label_size.x) * 0.5f,
                bb.Min.y + (sz.y - label_size.y) * 0.5f);
    dl->AddText(tpos, text_col, label);
    return pressed;
}

bool xp_button(const char* label, ImVec2 size) {
    return button_impl(label, size,
        IM_COL32(0xFC, 0xFC, 0xF0, 0xFF), IM_COL32(0xCF, 0xCE, 0xBB, 0xFF),
        IM_COL32(0xFF, 0xFF, 0xFA, 0xFF), IM_COL32(0xE0, 0xDE, 0xCC, 0xFF),
        IM_COL32(0xBF, 0xC4, 0xD1, 0xFF), IM_COL32(0xA5, 0xAC, 0xBE, 0xFF),
        IM_COL32(0x00, 0x00, 0x00, 0xFF),
        IM_COL32(0x64, 0x60, 0x54, 0xFF));
}

bool xp_green_button(const char* label, ImVec2 size) {
    return button_impl(label, size,
        IM_COL32(0x5D, 0xB9, 0x38, 0xFF), IM_COL32(0x24, 0x6A, 0x11, 0xFF),
        IM_COL32(0x7E, 0xD0, 0x50, 0xFF), IM_COL32(0x34, 0x8A, 0x1E, 0xFF),
        IM_COL32(0x38, 0x78, 0x20, 0xFF), IM_COL32(0x1C, 0x4A, 0x0C, 0xFF),
        IM_COL32(0xFF, 0xFF, 0xFF, 0xFF),
        IM_COL32(0x1B, 0x44, 0x0B, 0xFF));
}

bool xp_progress(const char* id, float value, ImVec2 size, float* out_value) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return false;
    ImGuiID iid = win->GetID(id);
    ImVec2 pos = win->DC.CursorPos;
    if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x;
    if (size.y <= 0) size.y = 16.f;
    ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(size);
    if (!ImGui::ItemAdd(bb, iid)) return false;
    bool hovered, held;
    ImGui::ButtonBehavior(bb, iid, &hovered, &held,
                          ImGuiButtonFlags_PressedOnClick);

    ImDrawList* dl = win->DrawList;
    // Sunken track
    dl->AddRectFilled(bb.Min, bb.Max, IM_COL32(0xE8, 0xE8, 0xE0, 0xFF), 3.f);
    dl->AddRect(bb.Min, bb.Max, IM_COL32(0x80, 0x80, 0x80, 0xFF), 3.f, 0, 1.0f);
    // Fill
    value = std::clamp(value, 0.f, 1.f);
    float fill_w = (bb.Max.x - bb.Min.x - 2.f) * value;
    if (fill_w > 0.f) {
        ImVec2 fmin(bb.Min.x + 1, bb.Min.y + 1);
        ImVec2 fmax(bb.Min.x + 1 + fill_w, bb.Max.y - 1);
        ImU32 top = IM_COL32(0x8E, 0xC9, 0xFB, 0xFF);
        ImU32 mid = IM_COL32(0x2F, 0x8E, 0xE6, 0xFF);
        ImU32 bot = IM_COL32(0x11, 0x5A, 0xB0, 0xFF);
        float h = fmax.y - fmin.y;
        ImVec2 mp(fmax.x, fmin.y + h * 0.5f);
        dl->AddRectFilledMultiColor(fmin, mp, top, top, mid, mid);
        dl->AddRectFilledMultiColor(ImVec2(fmin.x, mp.y), fmax, mid, mid, bot, bot);
        // Thin gloss highlight
        dl->AddRectFilledMultiColor(
            fmin, ImVec2(fmax.x, fmin.y + (h * 0.35f)),
            IM_COL32(255, 255, 255, 120), IM_COL32(255, 255, 255, 120),
            IM_COL32(255, 255, 255, 0),   IM_COL32(255, 255, 255, 0));
    }

    bool scrubbed = false;
    if (held || (hovered && ImGui::IsMouseDown(0))) {
        float mx = ImGui::GetIO().MousePos.x;
        float t = (mx - bb.Min.x) / (bb.Max.x - bb.Min.x);
        t = std::clamp(t, 0.f, 1.f);
        if (out_value) *out_value = t;
        scrubbed = true;
    }
    return scrubbed;
}

} // namespace theme
