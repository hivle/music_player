#pragma once
#include <string>
#include "miniaudio.h"

class Player {
public:
    Player();
    ~Player();

    bool load(const std::string& path);
    void play();
    void pause();
    void toggle();
    void stop();
    void set_volume(float v);
    void seek_seconds(double s);

    bool is_playing() const { return playing_ && !paused_; }
    bool is_paused() const { return paused_; }
    bool is_loaded() const { return loaded_; }
    bool reached_end() const;

    double position_seconds() const;
    double duration_seconds() const;
    float volume() const { return volume_; }
    const std::string& current_path() const { return path_; }

private:
    void unload();

    ma_engine engine_{};
    ma_sound sound_{};
    bool engine_ready_ = false;
    bool loaded_ = false;
    bool playing_ = false;
    bool paused_ = false;
    float volume_ = 0.8f;
    std::string path_;
};
