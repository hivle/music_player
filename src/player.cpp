#include "player.h"
#include <cstdio>

Player::Player() {
    if (ma_engine_init(nullptr, &engine_) == MA_SUCCESS) {
        engine_ready_ = true;
        ma_engine_set_volume(&engine_, volume_);
    } else {
        std::fprintf(stderr, "miniaudio: engine init failed\n");
    }
}

Player::~Player() {
    unload();
    if (engine_ready_) ma_engine_uninit(&engine_);
}

void Player::unload() {
    if (loaded_) {
        ma_sound_uninit(&sound_);
        loaded_ = false;
        playing_ = false;
        paused_ = false;
    }
}

bool Player::load(const std::string& path) {
    if (!engine_ready_) return false;
    unload();
    // MA_SOUND_FLAG_STREAM avoids fully decoding the file up front, so skipping
    // to a new track is near-instant even for large FLAC/WAV files.
    ma_uint32 flags = MA_SOUND_FLAG_STREAM;
    ma_result r = ma_sound_init_from_file(&engine_, path.c_str(), flags, nullptr, nullptr, &sound_);
    if (r != MA_SUCCESS) {
        std::fprintf(stderr, "miniaudio: failed to load %s (%d)\n", path.c_str(), (int)r);
        return false;
    }
    loaded_ = true;
    path_ = path;
    return true;
}

void Player::play() {
    if (!loaded_) return;
    ma_sound_start(&sound_);
    playing_ = true;
    paused_ = false;
}

void Player::pause() {
    if (!loaded_ || !playing_) return;
    ma_sound_stop(&sound_);
    paused_ = true;
}

void Player::toggle() {
    if (!loaded_) return;
    if (playing_ && !paused_) {
        pause();
    } else if (paused_) {
        ma_sound_start(&sound_);
        paused_ = false;
    } else {
        play();
    }
}

void Player::stop() {
    if (!loaded_) return;
    ma_sound_stop(&sound_);
    ma_sound_seek_to_pcm_frame(&sound_, 0);
    playing_ = false;
    paused_ = false;
}

void Player::set_volume(float v) {
    if (v < 0.f) v = 0.f;
    if (v > 1.f) v = 1.f;
    volume_ = v;
    if (engine_ready_) ma_engine_set_volume(&engine_, v);
}

void Player::seek_seconds(double s) {
    if (!loaded_) return;
    ma_uint32 sample_rate = ma_engine_get_sample_rate(&engine_);
    ma_uint64 frame = (ma_uint64)(s * sample_rate);
    ma_sound_seek_to_pcm_frame(&sound_, frame);
}

double Player::position_seconds() const {
    if (!loaded_) return 0.0;
    float c = 0.f;
    ma_sound_get_cursor_in_seconds(const_cast<ma_sound*>(&sound_), &c);
    return (double)c;
}

double Player::duration_seconds() const {
    if (!loaded_) return 0.0;
    float l = 0.f;
    ma_sound_get_length_in_seconds(const_cast<ma_sound*>(&sound_), &l);
    return (double)l;
}

bool Player::reached_end() const {
    if (!loaded_ || !playing_ || paused_) return false;
    return ma_sound_at_end(const_cast<ma_sound*>(&sound_)) != 0;
}
