#include "library.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mp4coverart.h>
#include <taglib/vorbisfile.h>
#include <taglib/opusfile.h>
#include <taglib/xiphcomment.h>

namespace fs = std::filesystem;

namespace library {

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool is_audio_file(const std::string& path) {
    std::string ext = lower(fs::path(path).extension().string());
    return ext == ".mp3" || ext == ".flac" || ext == ".wav" ||
           ext == ".ogg" || ext == ".opus" || ext == ".m4a" ||
           ext == ".aac";
}

std::vector<std::string> scan_audio(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        // path::string() on MSVC throws filesystem_error when the native
        // wide path contains characters not representable in the active
        // codepage. u8string() is guaranteed to produce UTF-8 without
        // throwing, which is what TagLib / miniaudio expect on Windows.
        try {
            std::string p = it->path().u8string();
            if (is_audio_file(p)) out.push_back(std::move(p));
        } catch (...) {
            // skip unrepresentable paths
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> find_playlists(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return out;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        try {
            std::string ext = lower(e.path().extension().u8string());
            if (ext == ".m3u" || ext == ".m3u8") out.push_back(e.path().u8string());
        } catch (...) {}
    }
    std::sort(out.begin(), out.end());
    return out;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> parse_m3u(const std::string& m3u_path) {
    std::vector<std::string> out;
    std::ifstream f(m3u_path);
    if (!f) return out;
    fs::path base = fs::path(m3u_path).parent_path();
    std::string line;
    while (std::getline(f, line)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        try {
            fs::path p = fs::u8path(s);
            if (p.is_relative()) p = base / p;
            std::error_code ec;
            fs::path canon = fs::weakly_canonical(p, ec);
            if (ec) canon = p;
            std::string u8 = canon.u8string();
            if (fs::exists(canon, ec) && is_audio_file(u8)) {
                out.push_back(std::move(u8));
            }
        } catch (...) {}
    }
    return out;
}

Track read_track(const std::string& path) {
    Track t;
    t.path = path;
    t.title = fs::path(path).stem().string();
    TagLib::FileRef f(path.c_str());
    if (!f.isNull()) {
        if (auto* tag = f.tag()) {
            auto title = tag->title().toCString(true);
            auto artist = tag->artist().toCString(true);
            auto album = tag->album().toCString(true);
            if (title && *title) t.title = title;
            if (artist) t.artist = artist;
            if (album) t.album = album;
        }
        if (auto* ap = f.audioProperties()) {
            t.duration_seconds = ap->lengthInMilliseconds() / 1000.0;
        }
    }
    return t;
}

static AlbumArt from_bytes(const TagLib::ByteVector& bv, const std::string& mime = "") {
    AlbumArt a;
    a.data.assign(bv.data(), bv.data() + bv.size());
    a.mime = mime;
    return a;
}

AlbumArt read_cover_art(const std::string& path) {
    AlbumArt none;
    std::string ext = lower(fs::path(path).extension().string());

    if (ext == ".mp3") {
        TagLib::MPEG::File file(path.c_str());
        TagLib::ID3v2::Tag* id3 = file.ID3v2Tag();
        if (id3) {
            auto frames = id3->frameListMap()["APIC"];
            if (!frames.isEmpty()) {
                auto* apic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
                if (apic) {
                    return from_bytes(apic->picture(), apic->mimeType().to8Bit(true));
                }
            }
        }
    } else if (ext == ".flac") {
        TagLib::FLAC::File file(path.c_str());
        auto pics = file.pictureList();
        if (!pics.isEmpty()) {
            auto* p = pics.front();
            return from_bytes(p->data(), p->mimeType().to8Bit(true));
        }
    } else if (ext == ".m4a" || ext == ".aac") {
        TagLib::MP4::File file(path.c_str());
        auto* tag = file.tag();
        if (tag) {
            auto items = tag->itemMap();
            auto it = items.find("covr");
            if (it != items.end()) {
                auto covers = it->second.toCoverArtList();
                if (!covers.isEmpty()) {
                    auto& c = covers.front();
                    std::string mime = c.format() == TagLib::MP4::CoverArt::PNG ? "image/png" : "image/jpeg";
                    return from_bytes(c.data(), mime);
                }
            }
        }
    } else if (ext == ".ogg") {
        TagLib::Ogg::Vorbis::File file(path.c_str());
        auto* xiph = file.tag();
        if (xiph) {
            auto pics = xiph->pictureList();
            if (!pics.isEmpty()) {
                auto* p = pics.front();
                return from_bytes(p->data(), p->mimeType().to8Bit(true));
            }
        }
    } else if (ext == ".opus") {
        TagLib::Ogg::Opus::File file(path.c_str());
        auto* xiph = file.tag();
        if (xiph) {
            auto pics = xiph->pictureList();
            if (!pics.isEmpty()) {
                auto* p = pics.front();
                return from_bytes(p->data(), p->mimeType().to8Bit(true));
            }
        }
    }
    return none;
}

} // namespace library
