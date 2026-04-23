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

// Windows's <GL/gl.h> is frozen at GL 1.1 and omits some 1.2+ constants.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "stb_image.h"

#include "library.h"
#include "platform.h"
#include "player.h"
#include "theme.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
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

static bool upload_rgba_to_texture(const unsigned char* pixels, int w, int h, ArtTexture& tex) {
    tex.destroy();
    if (!pixels || w <= 0 || h <= 0) return false;
    glGenTextures(1, &tex.id);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    tex.w = w; tex.h = h;
    return true;
}

// ---------------------------------------------------------------------------
// ArtLoader: reads cover art + stb_image-decodes it on a worker thread, so
// clicking a track doesn't stall the UI on big embedded covers. The UI
// thread polls for results and uploads them to GL (GL calls must stay on
// the main thread).
// ---------------------------------------------------------------------------
class ArtLoader {
public:
    struct Result {
        std::string path;
        std::vector<unsigned char> pixels;  // RGBA, w*h*4 bytes
        int w = 0, h = 0;
        bool empty = true;  // true means "no art for this track"
    };

    ArtLoader() {
        worker_ = std::thread([this] { run(); });
    }
    ~ArtLoader() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            quit_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    // Queue a decode for `path`. Any prior in-flight result is invalidated.
    void request(const std::string& path) {
        std::lock_guard<std::mutex> lk(mtx_);
        ++request_id_;
        pending_path_ = path;
        result_ready_ = false;
        cv_.notify_one();
    }

    // Cancel any pending / in-flight result.
    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        ++request_id_;
        pending_path_.clear();
        result_ready_ = false;
    }

    bool poll(Result& out) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!result_ready_) return false;
        out = std::move(result_);
        result_ready_ = false;
        return true;
    }

private:
    void run() {
        while (true) {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&]{ return quit_ || !pending_path_.empty(); });
            if (quit_) return;
            std::string path = std::move(pending_path_);
            pending_path_.clear();
            uint64_t id = request_id_;
            lk.unlock();

            Result r;
            r.path = path;
            AlbumArt art = library::read_cover_art(path);
            if (art.valid()) {
                int w, h, n;
                unsigned char* px = stbi_load_from_memory(
                    art.data.data(), (int)art.data.size(), &w, &h, &n, 4);
                if (px) {
                    r.pixels.assign(px, px + (size_t)w * h * 4);
                    r.w = w; r.h = h; r.empty = false;
                    stbi_image_free(px);
                }
            }

            lk.lock();
            // Drop if a newer request came in while we were decoding.
            if (id == request_id_) {
                result_ = std::move(r);
                result_ready_ = true;
            }
        }
    }

    std::thread worker_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool quit_ = false;

    std::string pending_path_;
    uint64_t request_id_ = 0;

    bool result_ready_ = false;
    Result result_;
};

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

    // Move tracks_[from] to the slot currently occupied by tracks_[to].
    // Returns the new index of the moved item, or -1 on bad input.
    int move_track(int from, int to) {
        std::lock_guard<std::mutex> lk(mtx_);
        int n = (int)tracks_.size();
        if (from < 0 || from >= n || to < 0 || to >= n || from == to) return -1;
        Track moving = std::move(tracks_[from]);
        tracks_.erase(tracks_.begin() + from);
        int dst = (from < to) ? to - 1 : to;
        if (dst > (int)tracks_.size()) dst = (int)tracks_.size();
        tracks_.insert(tracks_.begin() + dst, std::move(moving));
        return dst;
    }

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

    // Load a Unicode-capable system font setup so tags/filenames in non-Latin
    // scripts render as real glyphs instead of "?" boxes. We do this in two
    // passes: a Latin-friendly primary font first, then a CJK font merged on
    // top of the same ImFont — that way Chinese / Japanese / Korean glyphs
    // fall through to a font that actually contains them, without dropping
    // the nicer Latin rendering.
    //
    // Hint a generous atlas width so 20k+ CJK glyphs don't get silently
    // clipped on GPUs that default to a small texture limit.
    io.Fonts->TexDesiredWidth = 4096;
    const float FONT_PX = 15.0f;
    {
        // Primary ranges: everything that fits comfortably in a non-CJK font.
        static ImVector<ImWchar> latin_ranges;
        ImFontGlyphRangesBuilder lb;
        lb.AddRanges(io.Fonts->GetGlyphRangesDefault());
        lb.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
        lb.AddRanges(io.Fonts->GetGlyphRangesGreek());
        lb.AddRanges(io.Fonts->GetGlyphRangesVietnamese());
        lb.AddRanges(io.Fonts->GetGlyphRangesThai());
        lb.BuildRanges(&latin_ranges);

        ImFont* primary = nullptr;
        for (const auto& path : platform::find_ui_fonts()) {
            ImFontConfig cfg;
            cfg.OversampleH = 1;
            cfg.OversampleV = 1;
            cfg.PixelSnapH = true;
            primary = io.Fonts->AddFontFromFileTTF(path.c_str(), FONT_PX, &cfg, latin_ranges.Data);
            if (primary) {
                std::fprintf(stderr, "[fonts] primary: %s\n", path.c_str());
                break;
            }
        }
        if (!primary) {
            std::fprintf(stderr, "[fonts] primary: (fallback to ImGui default)\n");
            io.Fonts->AddFontDefault();
        }

        // CJK merged on top of the primary. Include explicit kana +
        // full-width punctuation ranges in case the ImGui helper misses any.
        static ImVector<ImWchar> cjk_ranges;
        ImFontGlyphRangesBuilder cb;
        cb.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        cb.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
        cb.AddRanges(io.Fonts->GetGlyphRangesKorean());
        static const ImWchar extra_cjk[] = {
            0x3000, 0x303F,   // CJK Symbols and Punctuation
            0x3040, 0x309F,   // Hiragana
            0x30A0, 0x30FF,   // Katakana
            0x31F0, 0x31FF,   // Katakana Phonetic Extensions
            0xFF00, 0xFFEF,   // Halfwidth and Fullwidth Forms
            0,
        };
        cb.AddRanges(extra_cjk);
        cb.BuildRanges(&cjk_ranges);

        ImFont* cjk = nullptr;
        for (const auto& path : platform::find_cjk_fonts()) {
            ImFontConfig cfg;
            cfg.OversampleH = 1;
            cfg.OversampleV = 1;
            cfg.PixelSnapH = true;
            cfg.MergeMode = true; // merges into whichever primary was added above
            cjk = io.Fonts->AddFontFromFileTTF(path.c_str(), FONT_PX, &cfg, cjk_ranges.Data);
            if (cjk) {
                std::fprintf(stderr, "[fonts] CJK: %s\n", path.c_str());
                break;
            }
        }
        if (!cjk) {
            std::fprintf(stderr, "[fonts] CJK: NONE FOUND "
                                 "(install fonts-noto-cjk or fonts-wqy-zenhei "
                                 "or fonts-ipafont-gothic to see Chinese/Japanese/Korean)\n");
        }
    }

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
    ArtLoader art_loader;
    int current_track = -1;
    ArtTexture art;
    std::string art_path; // path the current `art` texture belongs to

    // Tracks whose miniaudio load failed. We auto-skip them when advancing
    // through the list and show a "Unplayable" marker in the UI.
    std::unordered_set<std::string> unplayable;
    std::string last_error; // shown in status line

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
        std::string path;
        int n = 0;
        {
            std::lock_guard<std::mutex> lk(loader.mutex());
            const auto& tracks = loader.tracks();
            n = (int)tracks.size();
            if (idx < 0 || idx >= n) return;
            path = tracks[idx].path;
        }
        current_track = idx;
        if (!player.load(path)) {
            unplayable.insert(path);
            last_error = "Could not play: " + fs::path(path).filename().string();
            return;
        }
        unplayable.erase(path);
        last_error.clear();
        player.play();
        push_history(path);
        // Clear any previous cover immediately so the old track's art doesn't
        // briefly show for the new track while the decoder is still running.
        if (art_path != path) { art.destroy(); art_path.clear(); }
        // Decode cover art off the UI thread so switching tracks feels
        // instant even for files with large embedded covers.
        art_loader.request(path);
        if (shuffle_mode != ShuffleMode::Off) {
            auto it = std::find(shuffle_order.begin(), shuffle_order.end(), idx);
            if (it != shuffle_order.end())
                shuffle_pos = (int)std::distance(shuffle_order.begin(), it);
        }
    };

    // Look at tracks[i].path without long-lived locking.
    auto path_at = [&](int i) -> std::string {
        std::lock_guard<std::mutex> lk(loader.mutex());
        const auto& tracks = loader.tracks();
        if (i < 0 || i >= (int)tracks.size()) return "";
        return tracks[i].path;
    };

    auto next_track = [&]() {
        int n;
        {
            std::lock_guard<std::mutex> lk(loader.mutex());
            n = (int)loader.tracks().size();
        }
        if (n == 0) return;
        if (shuffle_mode == ShuffleMode::Off) {
            // Scan forward for the next playable track.
            for (int i = current_track + 1; i < n; ++i) {
                if (unplayable.count(path_at(i))) continue;
                play_index(i);
                return;
            }
        } else {
            if (shuffle_order.empty() || (int)shuffle_order.size() != n) rebuild_shuffle_for_current();
            int tried = 0;
            while (tried < (int)shuffle_order.size()) {
                shuffle_pos = (shuffle_pos + 1) % (int)shuffle_order.size();
                if (shuffle_pos == 0) {
                    std::lock_guard<std::mutex> lk(loader.mutex());
                    build_shuffle_order(loader.tracks(), shuffle_mode, shuffle_order);
                }
                int idx = shuffle_order[shuffle_pos];
                if (!unplayable.count(path_at(idx))) { play_index(idx); return; }
                ++tried;
            }
        }
    };

    auto prev_track = [&]() {
        if (shuffle_mode == ShuffleMode::Off) {
            for (int i = current_track - 1; i >= 0; --i) {
                if (unplayable.count(path_at(i))) continue;
                play_index(i);
                return;
            }
        } else if (!shuffle_order.empty()) {
            int tried = 0;
            while (tried < (int)shuffle_order.size()) {
                shuffle_pos = (shuffle_pos - 1 + (int)shuffle_order.size()) % (int)shuffle_order.size();
                int idx = shuffle_order[shuffle_pos];
                if (!unplayable.count(path_at(idx))) { play_index(idx); return; }
                ++tried;
            }
        }
    };

    // Window drag state for borderless titlebar. We track where the cursor
    // sat *inside the window* at drag-start, and each frame compute a new
    // window position that keeps that offset constant. Using the drag-start
    // window position here instead would feed back on itself (GLFW reports
    // cursor positions relative to the window, which itself just moved) and
    // cause 1-2 px jitter on every frame.
    bool dragging = false;
    double drag_offset_x = 0, drag_offset_y = 0;

    // Deferred actions (set during render, processed after so we don't re-lock
    // loader.mutex() in the middle of a render that already holds it).
    int pending_play = -1;
    bool pending_play_current = false;
    int pending_reorder_from = -1;
    int pending_reorder_to = -1;
    bool open_save_playlist_popup = false;
    char save_playlist_buf[256] = "queue.m3u";

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (player.reached_end()) next_track();

        // Upload any art that finished decoding on the worker thread. We
        // only replace the current texture if the decoded path still matches
        // what the user is playing — otherwise a stale decode would briefly
        // flash the wrong cover.
        {
            ArtLoader::Result r;
            while (art_loader.poll(r)) {
                std::string cur;
                {
                    std::lock_guard<std::mutex> lk(loader.mutex());
                    const auto& tracks = loader.tracks();
                    if (current_track >= 0 && current_track < (int)tracks.size())
                        cur = tracks[current_track].path;
                }
                if (r.path != cur) continue;
                if (r.empty) { art.destroy(); art_path.clear(); }
                else {
                    upload_rgba_to_texture(r.pixels.data(), r.w, r.h, art);
                    art_path = r.path;
                }
            }
        }

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
            glfwGetCursorPos(window, &drag_offset_x, &drag_offset_y);
        }
        if (dragging && mouse_down) {
            double cx, cy;
            glfwGetCursorPos(window, &cx, &cy);
            int wx, wy;
            glfwGetWindowPos(window, &wx, &wy);
            glfwSetWindowPos(window,
                             wx + (int)(cx - drag_offset_x),
                             wy + (int)(cy - drag_offset_y));
        }
        if (mouse_released) dragging = false;

        // --- Body -----------------------------------------------------------
        ImGui::SetCursorPos(ImVec2(0, TITLE_H));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        ImGui::BeginChild("##body", ImVec2(wsz.x, wsz.y - TITLE_H), false);

        // Top row: folder + load
        ImGui::Text("Folder");
        ImGui::SameLine();
        ImGui::PushItemWidth(-420);
        if (ImGui::InputText("##folder", folder_buf, sizeof(folder_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            folder = folder_buf;
            reload_library();
            rebuild_track_list();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (theme::xp_button("Browse...", ImVec2(90, 24))) {
            std::string picked = platform::pick_folder(folder);
            if (!picked.empty()) {
                folder = picked;
                std::strncpy(folder_buf, folder.c_str(), sizeof(folder_buf) - 1);
                folder_buf[sizeof(folder_buf) - 1] = '\0';
                reload_library();
                rebuild_track_list();
            }
        }
        ImGui::SameLine();
        if (theme::xp_button("Load", ImVec2(70, 24))) {
            folder = folder_buf;
            reload_library();
            rebuild_track_list();
        }
        ImGui::SameLine();
        if (theme::xp_button("Save Playlist", ImVec2(120, 24))) {
            open_save_playlist_popup = true;
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

        // Bottom pane: fixed-height row with album art + metadata on the left,
        // progress bar and transport controls stacked on the right. Everything
        // above it is the playlist.
        const float ART_SIZE    = 104.f;
        const float BOTTOM_H    = ART_SIZE + 16.f; // art + a little padding
        const float PROG_H      = 22.f;
        const float TRANS_H     = 34.f;
        const float SPACING     = 4.f;
        const float FOOTER_H    = BOTTOM_H + PROG_H + TRANS_H + SPACING * 3.f;

        // --- Playlist (top, full width) ---------------------------------
        float list_h = ImGui::GetContentRegionAvail().y - FOOTER_H;
        if (list_h < 120) list_h = 120;
        {
            float list_w = ImGui::GetContentRegionAvail().x;
            ImVec2 panel_min = ImGui::GetCursorScreenPos();
            ImVec2 panel_max(panel_min.x + list_w, panel_min.y + list_h);
            theme::draw_sunken_panel(dl, panel_min, panel_max, IM_COL32(255, 255, 255, 255));

            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(255, 255, 255, 255));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
            ImGui::BeginChild("##tracks", ImVec2(list_w, list_h), false);

            std::lock_guard<std::mutex> lk(loader.mutex());
            const auto& tracks = loader.tracks();

            if (tracks.empty()) {
                ImGui::TextDisabled("No tracks.");
            } else {
                // Clipper + drag-and-drop don't mix well because the payload
                // source/target rows must exist during the same frame. Only
                // use the clipper when no drag is in progress.
                bool dragging_payload = ImGui::GetDragDropPayload() != nullptr;
                auto render_row = [&](int i) {
                    const auto& t = tracks[i];
                    std::string label = t.artist.empty()
                        ? t.title
                        : (t.artist + " - " + t.title);
                    bool bad = unplayable.count(t.path) > 0;
                    char line[512];
                    std::snprintf(line, sizeof(line), "%4d. %s%s",
                                  i + 1, label.c_str(),
                                  bad ? "   [Unplayable]" : "");
                    bool sel = (i == current_track);
                    ImGui::PushID(i);
                    if (bad) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(170, 50, 50, 255));
                    if (ImGui::Selectable(line, sel, ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (ImGui::IsMouseDoubleClicked(0)) pending_play = i;
                        else current_track = i;
                    }
                    if (bad) ImGui::PopStyleColor();

                    // Drag source: picks the row up.
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        ImGui::SetDragDropPayload("TRACK_ROW", &i, sizeof(int));
                        ImGui::Text("Move: %s", label.c_str());
                        ImGui::EndDragDropSource();
                    }
                    // Drop target: places the dragged row before this one.
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("TRACK_ROW")) {
                            int from = *(const int*)p->Data;
                            pending_reorder_from = from;
                            pending_reorder_to = i;
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::PopID();
                };

                if (dragging_payload) {
                    for (int i = 0; i < (int)tracks.size(); ++i) render_row(i);
                } else {
                    ImGuiListClipper clipper;
                    clipper.Begin((int)tracks.size());
                    while (clipper.Step()) {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                            render_row(i);
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        // --- Bottom row: album art + metadata ---------------------------
        ImGui::Dummy(ImVec2(0, SPACING));
        {
            ImVec2 art_min = ImGui::GetCursorScreenPos();
            ImVec2 art_max(art_min.x + ART_SIZE, art_min.y + ART_SIZE);
            theme::draw_sunken_panel(dl, art_min, art_max, IM_COL32(255, 255, 255, 255));
            if (art.id) {
                dl->AddImage((ImTextureID)(intptr_t)art.id,
                             ImVec2(art_min.x + 3, art_min.y + 3),
                             ImVec2(art_max.x - 3, art_max.y - 3));
            } else {
                ImVec2 ts = ImGui::CalcTextSize("(no art)");
                dl->AddText(ImVec2(art_min.x + (ART_SIZE - ts.x) * 0.5f,
                                   art_min.y + (ART_SIZE - ts.y) * 0.5f),
                            IM_COL32(150, 150, 150, 255), "(no art)");
            }
            ImGui::Dummy(ImVec2(ART_SIZE, ART_SIZE));

            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(0, 2));
            {
                std::lock_guard<std::mutex> lk(loader.mutex());
                const auto& tracks = loader.tracks();
                if (current_track >= 0 && current_track < (int)tracks.size()) {
                    const auto& t = tracks[current_track];
                    float wrap_w = ImGui::GetContentRegionAvail().x - 8.f;
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
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
            ImGui::EndGroup();
        }

        // --- Progress / seek bar ---------------------------------------
        ImGui::Dummy(ImVec2(0, SPACING));
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

        // --- Transport + fast skip + volume ----------------------------
        // Fast skip jumps by FAST_SKIP_SECONDS within the current track.
        // Keyboard shortcuts: Left / Right arrows, Space toggles play.
        constexpr double FAST_SKIP_SECONDS = 10.0;
        auto fast_skip = [&](double delta) {
            if (!player.is_loaded()) return;
            double d = player.duration_seconds();
            double np = player.position_seconds() + delta;
            if (np < 0) np = 0;
            if (d > 0 && np > d - 0.25) np = std::max(0.0, d - 0.25);
            player.seek_seconds(np);
        };

        ImGui::Dummy(ImVec2(0, SPACING));
        ImVec2 btn(52, 28);
        if (theme::xp_button("|<<", btn)) prev_track();
        ImGui::SameLine();
        if (theme::xp_button("<< 10s", ImVec2(68, 28))) fast_skip(-FAST_SKIP_SECONDS);
        ImGui::SameLine();
        const char* play_label = player.is_playing() ? "|| Pause" : "> Play";
        if (theme::xp_green_button(play_label, ImVec2(100, 28))) {
            if (!player.is_loaded()) pending_play_current = true;
            else player.toggle();
        }
        ImGui::SameLine();
        if (theme::xp_button("10s >>", ImVec2(68, 28))) fast_skip(FAST_SKIP_SECONDS);
        ImGui::SameLine();
        if (theme::xp_button(">>|", btn)) next_track();
        ImGui::SameLine();
        if (theme::xp_button("[ ] Stop", ImVec2(76, 28))) player.stop();

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(12, 0));
        ImGui::SameLine();
        ImGui::Text("Vol");
        ImGui::SameLine();
        float vol = player.volume();
        ImGui::PushItemWidth(100);
        if (ImGui::SliderFloat("##vol", &vol, 0.f, 1.f, "")) player.set_volume(vol);
        ImGui::PopItemWidth();

        // Keyboard shortcuts — only when no text field has focus, so typing
        // in the Folder input doesn't trigger seeks.
        if (!ImGui::IsAnyItemActive()) {
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) fast_skip(FAST_SKIP_SECONDS);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))  fast_skip(-FAST_SKIP_SECONDS);
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                if (!player.is_loaded()) pending_play_current = true;
                else player.toggle();
            }
        }

        // Status line under controls.
        if (!last_error.empty()) {
            ImGui::TextColored(ImVec4(0.65f, 0.10f, 0.10f, 1.f), "%s", last_error.c_str());
        }

        // Save-playlist popup.
        if (open_save_playlist_popup) {
            ImGui::OpenPopup("Save Playlist");
            open_save_playlist_popup = false;
        }
        ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Save Playlist", nullptr,
                                    ImGuiWindowFlags_NoResize)) {
            ImGui::Text("Save current queue to an .m3u file in:");
            ImGui::TextWrapped("%s", folder.c_str());
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##name", save_playlist_buf, sizeof(save_playlist_buf));
            ImGui::Dummy(ImVec2(0, 4));
            bool do_save = false;
            if (theme::xp_button("Save", ImVec2(90, 26))) do_save = true;
            ImGui::SameLine();
            if (theme::xp_button("Cancel", ImVec2(90, 26))) ImGui::CloseCurrentPopup();
            if (do_save) {
                std::string name = save_playlist_buf;
                if (!name.empty()) {
                    // Force a .m3u extension if missing.
                    std::string lower_name;
                    lower_name.reserve(name.size());
                    for (char c : name) lower_name.push_back((char)std::tolower((unsigned char)c));
                    if (lower_name.find(".m3u") == std::string::npos &&
                        lower_name.find(".m3u8") == std::string::npos) {
                        name += ".m3u";
                    }
                    fs::path out_path = fs::path(folder) / fs::u8path(name);
                    std::ofstream out(out_path, std::ios::binary);
                    if (out) {
                        out << "#EXTM3U\n";
                        std::lock_guard<std::mutex> lk(loader.mutex());
                        for (const auto& t : loader.tracks()) {
                            int secs = (int)t.duration_seconds;
                            std::string disp = t.artist.empty()
                                ? t.title
                                : (t.artist + " - " + t.title);
                            out << "#EXTINF:" << secs << "," << disp << "\n";
                            out << t.path << "\n";
                        }
                    }
                    // Refresh source list so the new playlist appears.
                    playlists = library::find_playlists(folder);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::End();

        // Process deferred actions (outside of any held loader mutex).
        if (pending_reorder_from >= 0 && pending_reorder_to >= 0) {
            // Record which track was playing so we can follow it through
            // the reorder. Anchor by path: indices shift as items move.
            std::string playing_path;
            {
                std::lock_guard<std::mutex> lk(loader.mutex());
                const auto& tracks = loader.tracks();
                if (current_track >= 0 && current_track < (int)tracks.size())
                    playing_path = tracks[current_track].path;
            }
            int moved = loader.move_track(pending_reorder_from, pending_reorder_to);
            (void)moved;
            if (!playing_path.empty()) {
                std::lock_guard<std::mutex> lk(loader.mutex());
                const auto& tracks = loader.tracks();
                for (int i = 0; i < (int)tracks.size(); ++i) {
                    if (tracks[i].path == playing_path) { current_track = i; break; }
                }
            }
            // Rebuild shuffle order to reflect the new track list.
            rebuild_shuffle_for_current();
            pending_reorder_from = pending_reorder_to = -1;
        }
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
