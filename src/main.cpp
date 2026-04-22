// XP Luna style music player — main entry.
// Thin wrapper around GLFW + OpenGL + ImGui with an async metadata loader,
// bounded play history, and iPod-style shuffle modes.

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#endif

#include <GLFW/glfw3.h>

#ifdef __APPLE__
    #include <OpenGL/gl.h>
#else
    #include <GL/gl.h>
#endif

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "stb_image.h"

#include "library.h"
#include "player.h"
#include "theme.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Album art texture (single held at a time to keep RAM bounded).
// ---------------------------------------------------------------------------
struct ArtTexture {
    GLuint id = 0;
    int w = 0, h = 0;
    void destroy() {
        if (id) { glDeleteTextures(1, &id); id = 0; w = h = 0; }
    }
};

static bool upload_art_from_bytes(const std::vector<uint8_t>& bytes, ArtTexture& tex) {
    tex.destroy();
    if (bytes.empty()) return false;
    int w, h, n;
    unsigned char* pixels = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!pixels) return false;
    glGenTextures(1, &tex.id);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);
    tex.w = w; tex.h = h;
    return true;
}

// ---------------------------------------------------------------------------
// Async library loader: fills metadata on a background thread so the UI
// stays responsive even on large folders.
// ---------------------------------------------------------------------------
class LibraryLoader {
public:
    ~LibraryLoader() { cancel_and_join(); }

    void start(std::vector<std::string> paths) {
        cancel_and_join();
        cancel_.store(false);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            tracks_.clear();
            tracks_.reserve(paths.size());
            for (auto& p : paths) {
                Track t;
                t.path = p;
                t.title = fs::path(p).stem().string();
                tracks_.push_back(std::move(t));
            }
            loaded_count_.store(0);
        }
        total_ = paths.size();
        if (paths.empty()) return;
        thread_ = std::thread([this, paths = std::move(paths)]() {
            for (size_t i = 0; i < paths.size(); ++i) {
                if (cancel_.load()) return;
                Track full = library::read_track(paths[i]);
                std::lock_guard<std::mutex> lk(mtx_);
                if (i < tracks_.size() && tracks_[i].path == full.path) {
                    tracks_[i] = std::move(full);
                }
                loaded_count_.fetch_add(1);
            }
        });
    }

    // Caller takes the lock, reads tracks_, releases.
    std::mutex& mutex() { return mtx_; }
    const std::vector<Track>& tracks() const { return tracks_; }
    size_t loaded() const { return loaded_count_.load(); }
    size_t total() const { return total_; }

private:
    void cancel_and_join() {
        cancel_.store(true);
        if (thread_.joinable()) thread_.join();
    }

    std::thread thread_;
    std::atomic<bool> cancel_{false};
    std::atomic<size_t> loaded_count_{0};
    size_t total_ = 0;
    std::mutex mtx_;
    std::vector<Track> tracks_;
};

// ---------------------------------------------------------------------------
// Shuffle: iPod-style linear (shuffle once, play through) and Smart (same as
// linear but re-ordered to avoid adjacent same-artist tracks).
// ---------------------------------------------------------------------------
enum class ShuffleMode { Off, Linear, Smart };

static std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}

static void build_shuffle_order(const std::vector<Track>& tracks,
                                ShuffleMode mode,
                                std::vector<int>& out) {
    out.clear();
    out.resize(tracks.size());
    std::iota(out.begin(), out.end(), 0);
    if (mode == ShuffleMode::Off) return;
    std::shuffle(out.begin(), out.end(), rng());
    if (mode == ShuffleMode::Smart) {
        // Push adjacent same-artist tracks apart by swapping with a later
        // track with a different artist. Single pass, O(n*k) worst case.
        for (size_t i = 1; i < out.size(); ++i) {
            const std::string& prev = tracks[out[i - 1]].artist;
            if (prev.empty() || tracks[out[i]].artist != prev) continue;
            for (size_t j = i + 1; j < out.size(); ++j) {
                if (tracks[out[j]].artist != prev) { std::swap(out[i], out[j]); break; }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Misc helpers
// ---------------------------------------------------------------------------
static std::string format_time(double s) {
    if (s < 0 || s != s) s = 0;
    int total = (int)s;
    int m = total / 60;
    int sec = total % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
    return buf;
}

static void glfw_error_cb(int code, const char* msg) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, msg);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    glfwSetErrorCallback(glfw_error_cb);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

    GLFWwindow* window = glfwCreateWindow(860, 580, "Music Player", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 120");
    theme::apply_style();

    // --- App state -----------------------------------------------------------
    std::string folder = (argc > 1) ? argv[1] : fs::current_path().string();
    char folder_buf[1024];
    std::strncpy(folder_buf, folder.c_str(), sizeof(folder_buf) - 1);
    folder_buf[sizeof(folder_buf) - 1] = '\0';

    std::vector<std::string> all_audio;
    std::vector<std::string> playlists;
    int selected_source = 0;      // 0 = All tracks, 1..N = playlists, last = History

    LibraryLoader loader;
    Player player;
    int current_track = -1;
    ArtTexture art;

    ShuffleMode shuffle_mode = ShuffleMode::Off;
    std::vector<int> shuffle_order;
    int shuffle_pos = 0;

    std::deque<std::string> history;     // most-recent first
    constexpr size_t HISTORY_MAX = 200;

    auto push_history = [&](const std::string& path) {
        history.erase(std::remove(history.begin(), history.end(), path), history.end());
        history.push_front(path);
        while (history.size() > HISTORY_MAX) history.pop_back();
    };

    auto rebuild_shuffle_for_current = [&]() {
        std::lock_guard<std::mutex> lk(loader.mutex());
        build_shuffle_order(loader.tracks(), shuffle_mode, shuffle_order);
        if (current_track >= 0) {
            auto it = std::find(shuffle_order.begin(), shuffle_order.end(), current_track);
            shuffle_pos = (it == shuffle_order.end()) ? 0 : (int)std::distance(shuffle_order.begin(), it);
        } else {
            shuffle_pos = 0;
        }
    };

    auto reload_library = [&]() {
        all_audio = library::scan_audio(folder);
        playlists = library::find_playlists(folder);
        if (selected_source < 0) selected_source = 0;
    };

    auto source_paths = [&]() -> std::vector<std::string> {
        int last_history_idx = 1 + (int)playlists.size(); // index of "History"
        if (selected_source == 0) return all_audio;
        if (selected_source == last_history_idx) {
            return std::vector<std::string>(history.begin(), history.end());
        }
        int pi = selected_source - 1;
        if (pi >= 0 && pi < (int)playlists.size())
            return library::parse_m3u(playlists[pi]);
        return {};
    };

    auto rebuild_track_list = [&]() {
        current_track = -1;
        art.destroy();
        loader.start(source_paths());
        rebuild_shuffle_for_current();
    };

    reload_library();
    rebuild_track_list();

    auto play_index = [&](int idx) {
        std::lock_guard<std::mutex> lk(loader.mutex());
        const auto& tracks = loader.tracks();
        if (idx < 0 || idx >= (int)tracks.size()) return;
        current_track = idx;
        if (!player.load(tracks[idx].path)) return;
        player.play();
        push_history(tracks[idx].path);
        auto a = library::read_cover_art(tracks[idx].path);
        if (a.valid()) upload_art_from_bytes(a.data, art);
        else art.destroy();
        if (shuffle_mode != ShuffleMode::Off) {
            auto it = std::find(shuffle_order.begin(), shuffle_order.end(), idx);
            if (it != shuffle_order.end())
                shuffle_pos = (int)std::distance(shuffle_order.begin(), it);
        }
    };

    auto next_track = [&]() {
        int n;
        {
            std::lock_guard<std::mutex> lk(loader.mutex());
            n = (int)loader.tracks().size();
        }
        if (n == 0) return;
        if (shuffle_mode == ShuffleMode::Off) {
            int next = std::min(n - 1, current_track + 1);
            if (next != current_track) play_index(next);
        } else {
            if (shuffle_order.empty() || (int)shuffle_order.size() != n) rebuild_shuffle_for_current();
            shuffle_pos = (shuffle_pos + 1) % (int)shuffle_order.size();
            if (shuffle_pos == 0) {
                // completed a cycle: reshuffle for next round (iPod classic behaviour)
                std::lock_guard<std::mutex> lk(loader.mutex());
                build_shuffle_order(loader.tracks(), shuffle_mode, shuffle_order);
            }
            play_index(shuffle_order[shuffle_pos]);
        }
    };

    auto prev_track = [&]() {
        if (shuffle_mode == ShuffleMode::Off) {
            if (current_track > 0) play_index(current_track - 1);
        } else if (!shuffle_order.empty()) {
            shuffle_pos = (shuffle_pos - 1 + (int)shuffle_order.size()) % (int)shuffle_order.size();
            play_index(shuffle_order[shuffle_pos]);
        }
    };

    // Window drag state for borderless titlebar.
    bool dragging = false;
    double drag_cursor_x = 0, drag_cursor_y = 0;
    int drag_win_x = 0, drag_win_y = 0;

    // Deferred actions (set during render, processed after so we don't re-lock
    // loader.mutex() in the middle of a render that already holds it).
    int pending_play = -1;
    bool pending_play_current = false;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (player.reached_end()) next_track();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int fb_w, fb_h;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar(2);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 wsz = ImGui::GetWindowSize();

        // --- XP titlebar ----------------------------------------------------
        const float TITLE_H = 28.f;
        ImVec2 tb_min(wpos.x, wpos.y);
        ImVec2 tb_max(wpos.x + wsz.x, wpos.y + TITLE_H);

        std::string title = "Music Player";
        {
            std::lock_guard<std::mutex> lk(loader.mutex());
            const auto& tracks = loader.tracks();
            if (current_track >= 0 && current_track < (int)tracks.size()) {
                title = "Music Player  -  " + tracks[current_track].title;
                if (!tracks[current_track].artist.empty())
                    title += "  -  " + tracks[current_track].artist;
            }
        }
        theme::draw_titlebar(dl, tb_min, tb_max, title.c_str());

        const float BTN_W = 28.f, BTN_H = 20.f;
        ImVec2 close_min(tb_max.x - BTN_W - 4, tb_min.y + 4);
        ImVec2 close_max(close_min.x + BTN_W, close_min.y + BTN_H);
        ImVec2 min_min(close_min.x - BTN_W - 2, close_min.y);
        ImVec2 min_max(min_min.x + BTN_W, min_min.y + BTN_H);

        ImVec2 mouse = io.MousePos;
        auto in_rect = [&](ImVec2 a, ImVec2 b){
            return mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y;
        };
        bool close_hover = in_rect(close_min, close_max);
        bool min_hover = in_rect(min_min, min_max);

        auto draw_ctrl_btn = [&](ImVec2 a, ImVec2 b, bool hovered,
                                 ImU32 face_top, ImU32 face_bot, const char* glyph) {
            dl->AddRectFilledMultiColor(a, b,
                hovered ? IM_COL32(255, 200, 100, 255) : face_top,
                hovered ? IM_COL32(255, 200, 100, 255) : face_top,
                hovered ? IM_COL32(200, 60, 20, 255)   : face_bot,
                hovered ? IM_COL32(200, 60, 20, 255)   : face_bot);
            dl->AddRect(a, b, IM_COL32(0, 30, 100, 255), 2.f, 0, 1.0f);
            ImVec2 ts = ImGui::CalcTextSize(glyph);
            dl->AddText(ImVec2(a.x + ((b.x - a.x) - ts.x) * 0.5f,
                               a.y + ((b.y - a.y) - ts.y) * 0.5f - 1),
                        IM_COL32(255, 255, 255, 255), glyph);
        };
        draw_ctrl_btn(close_min, close_max, close_hover,
                      IM_COL32(0xEF, 0x6A, 0x5F, 0xFF), IM_COL32(0xB4, 0x1C, 0x1C, 0xFF), "X");
        draw_ctrl_btn(min_min, min_max, min_hover,
                      IM_COL32(0x5E, 0x9E, 0xE3, 0xFF), IM_COL32(0x11, 0x48, 0xA8, 0xFF), "_");

        bool mouse_clicked = ImGui::IsMouseClicked(0);
        bool mouse_down = ImGui::IsMouseDown(0);
        bool mouse_released = ImGui::IsMouseReleased(0);
        bool on_titlebar = mouse.y >= tb_min.y && mouse.y < tb_max.y;

        if (mouse_clicked && close_hover)        glfwSetWindowShouldClose(window, GLFW_TRUE);
        else if (mouse_clicked && min_hover)     glfwIconifyWindow(window);
        else if (mouse_clicked && on_titlebar && !close_hover && !min_hover) {
            dragging = true;
            glfwGetCursorPos(window, &drag_cursor_x, &drag_cursor_y);
            glfwGetWindowPos(window, &drag_win_x, &drag_win_y);
        }
        if (dragging && mouse_down) {
            double cx, cy;
            glfwGetCursorPos(window, &cx, &cy);
            glfwSetWindowPos(window,
                             drag_win_x + (int)(cx - drag_cursor_x),
                             drag_win_y + (int)(cy - drag_cursor_y));
        }
        if (mouse_released) dragging = false;

        // --- Body -----------------------------------------------------------
        ImGui::SetCursorPos(ImVec2(0, TITLE_H));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        ImGui::BeginChild("##body", ImVec2(wsz.x, wsz.y - TITLE_H), false);

        // Top row: folder + load
        ImGui::Text("Folder");
        ImGui::SameLine();
        ImGui::PushItemWidth(-200);
        if (ImGui::InputText("##folder", folder_buf, sizeof(folder_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            folder = folder_buf;
            reload_library();
            rebuild_track_list();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (theme::xp_button("Load", ImVec2(80, 24))) {
            folder = folder_buf;
            reload_library();
            rebuild_track_list();
        }
        ImGui::SameLine();
        ImGui::Text("%zu/%zu", loader.loaded(), loader.total());

        // Source combo (All tracks / playlists / History)
        std::vector<std::string> sources;
        sources.push_back("All tracks");
        for (auto& p : playlists) sources.push_back("Playlist: " + fs::path(p).filename().string());
        sources.push_back("History");
        if (selected_source >= (int)sources.size()) selected_source = 0;

        ImGui::PushItemWidth(200);
        if (ImGui::BeginCombo("Source", sources[selected_source].c_str())) {
            for (int i = 0; i < (int)sources.size(); ++i) {
                bool sel = (i == selected_source);
                if (ImGui::Selectable(sources[i].c_str(), sel)) {
                    selected_source = i;
                    rebuild_track_list();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(12, 0));
        ImGui::SameLine();
        ImGui::Text("Shuffle");
        ImGui::SameLine();
        const char* shuffle_names[] = { "Off", "Linear (iPod)", "Smart" };
        ImGui::PushItemWidth(140);
        int sm = (int)shuffle_mode;
        if (ImGui::Combo("##shuffle", &sm, shuffle_names, 3)) {
            shuffle_mode = (ShuffleMode)sm;
            rebuild_shuffle_for_current();
        }
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 2));

        // Middle: track list on left, art + metadata on right
        const float right_w = 260.f;
        float body_h = ImGui::GetContentRegionAvail().y - 100.f;
        if (body_h < 120) body_h = 120;

        // Track list
        {
            ImVec2 panel_min = ImGui::GetCursorScreenPos();
            float list_w = ImGui::GetContentRegionAvail().x - right_w - 8;
            ImVec2 panel_max(panel_min.x + list_w, panel_min.y + body_h);
            theme::draw_sunken_panel(dl, panel_min, panel_max, IM_COL32(255, 255, 255, 255));

            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(255, 255, 255, 255));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
            ImGui::BeginChild("##tracks", ImVec2(list_w, body_h), false);

            std::lock_guard<std::mutex> lk(loader.mutex());
            const auto& tracks = loader.tracks();

            if (tracks.empty()) {
                ImGui::TextDisabled("No tracks.");
            } else {
                ImGuiListClipper clipper;
                clipper.Begin((int)tracks.size());
                while (clipper.Step()) {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                        const auto& t = tracks[i];
                        std::string label = t.artist.empty()
                            ? t.title
                            : (t.artist + " - " + t.title);
                        char line[512];
                        std::snprintf(line, sizeof(line), "%4d. %s", i + 1, label.c_str());
                        bool sel = (i == current_track);
                        ImGui::PushID(i);
                        if (ImGui::Selectable(line, sel, ImGuiSelectableFlags_AllowDoubleClick)) {
                            if (ImGui::IsMouseDoubleClicked(0)) pending_play = i;
                            else current_track = i;
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        // Right: album art + metadata
        ImGui::SameLine();
        ImGui::BeginGroup();
        {
            ImVec2 art_min = ImGui::GetCursorScreenPos();
            float art_size = right_w - 20.f;
            ImVec2 art_max(art_min.x + art_size, art_min.y + art_size);
            theme::draw_sunken_panel(dl, art_min, art_max, IM_COL32(255, 255, 255, 255));
            if (art.id) {
                dl->AddImage((ImTextureID)(intptr_t)art.id,
                             ImVec2(art_min.x + 3, art_min.y + 3),
                             ImVec2(art_max.x - 3, art_max.y - 3));
            } else {
                ImVec2 ts = ImGui::CalcTextSize("(no album art)");
                dl->AddText(ImVec2(art_min.x + (art_size - ts.x) * 0.5f,
                                   art_min.y + (art_size - ts.y) * 0.5f),
                            IM_COL32(150, 150, 150, 255), "(no album art)");
            }
            ImGui::Dummy(ImVec2(art_size, art_size));
            ImGui::Spacing();
            {
                std::lock_guard<std::mutex> lk(loader.mutex());
                const auto& tracks = loader.tracks();
                if (current_track >= 0 && current_track < (int)tracks.size()) {
                    const auto& t = tracks[current_track];
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + art_size);
                    ImGui::TextColored(ImVec4(0, 0, 0, 1), "%s", t.title.c_str());
                    if (!t.artist.empty())
                        ImGui::TextColored(ImVec4(0.20f, 0.30f, 0.55f, 1), "%s", t.artist.c_str());
                    if (!t.album.empty())
                        ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.35f, 1), "%s", t.album.c_str());
                    ImGui::PopTextWrapPos();
                } else {
                    ImGui::TextDisabled("Nothing playing");
                }
            }
        }
        ImGui::EndGroup();

        // Seek bar
        ImGui::Dummy(ImVec2(0, 4));
        double pos = player.position_seconds();
        double dur = player.duration_seconds();
        if (dur <= 0.0 && current_track >= 0) {
            std::lock_guard<std::mutex> lk(loader.mutex());
            const auto& tracks = loader.tracks();
            if (current_track < (int)tracks.size())
                dur = tracks[current_track].duration_seconds;
        }
        float frac = (dur > 0.0) ? (float)std::clamp(pos / dur, 0.0, 1.0) : 0.f;
        ImGui::Text("%s", format_time(pos).c_str());
        ImGui::SameLine();
        float new_frac = frac;
        if (theme::xp_progress("##seek", frac,
                               ImVec2(ImGui::GetContentRegionAvail().x - 60, 18), &new_frac)) {
            if (dur > 0.0 && player.is_loaded()) player.seek_seconds((double)new_frac * dur);
        }
        ImGui::SameLine();
        ImGui::Text("%s", format_time(dur).c_str());

        // Transport
        ImGui::Dummy(ImVec2(0, 4));
        ImVec2 btn(64, 28);
        if (theme::xp_button("|<<", btn)) prev_track();
        ImGui::SameLine();
        const char* play_label = player.is_playing() ? "|| Pause" : "> Play";
        if (theme::xp_green_button(play_label, ImVec2(110, 28))) {
            if (!player.is_loaded()) pending_play_current = true;
            else player.toggle();
        }
        ImGui::SameLine();
        if (theme::xp_button(">>|", btn)) next_track();
        ImGui::SameLine();
        if (theme::xp_button("[ ] Stop", ImVec2(84, 28))) player.stop();

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(16, 0));
        ImGui::SameLine();
        ImGui::Text("Vol");
        ImGui::SameLine();
        float vol = player.volume();
        ImGui::PushItemWidth(120);
        if (ImGui::SliderFloat("##vol", &vol, 0.f, 1.f, "")) player.set_volume(vol);
        ImGui::PopItemWidth();

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::End();

        // Process deferred actions (outside of any held loader mutex).
        if (pending_play >= 0) {
            play_index(pending_play);
            pending_play = -1;
        }
        if (pending_play_current) {
            pending_play_current = false;
            int start = -1;
            {
                std::lock_guard<std::mutex> lk(loader.mutex());
                if (!loader.tracks().empty()) {
                    start = (current_track >= 0) ? current_track : 0;
                    if (shuffle_mode != ShuffleMode::Off && !shuffle_order.empty())
                        start = shuffle_order[shuffle_pos];
                }
            }
            if (start >= 0) play_index(start);
        }

        // Render
        ImGui::Render();
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0xEC / 255.f, 0xE9 / 255.f, 0xD8 / 255.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    art.destroy();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
