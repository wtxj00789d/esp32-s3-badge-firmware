#pragma once

#include <atomic>

#include "audio_codec.h"

class BadgeSoundPlayer {
public:
    explicit BadgeSoundPlayer(AudioCodec& audio);

    bool Play(const char* path);
    void Stop();
    bool playing() const { return playing_; }

private:
    AudioCodec& audio_;
    std::atomic<bool> playing_ { false };
    std::atomic<bool> stop_requested_ { false };
};
