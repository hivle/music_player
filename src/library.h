#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct Track {
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    double duration_seconds = 0.0;
};

struct AlbumArt {
    std::vector<uint8_t> data;
    std::string mime;
    bool valid() const { return !data.empty(); }
};

namespace library {

bool is_audio_file(const std::string& path);

// Recursively collect audio files under dir.
std::vector<std::string> scan_audio(const std::string& dir);

// List .m3u / .m3u8 files directly inside dir.
std::vector<std::string> find_playlists(const std::string& dir);

// Parse an m3u/m3u8 file. Relative paths are resolved against the file's folder.
std::vector<std::string> parse_m3u(const std::string& m3u_path);

// Fill title/artist/album/duration from file tags. Falls back to filename for title.
Track read_track(const std::string& path);

// Return embedded cover art (PNG/JPEG bytes) or an empty AlbumArt on failure.
AlbumArt read_cover_art(const std::string& path);

} // namespace library
