#pragma once

#include <atomic>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio_codec.h"

class BadgeSoundPlayer {
public:
    explicit BadgeSoundPlayer(AudioCodec& audio);

    bool Play(const char* path);
    bool PlayPcm(const int16_t* samples, size_t sample_count);
    void Stop();
    bool playing() const { return playing_; }

private:
    enum class Source {
        None,
        File,
        Pcm,
    };

    AudioCodec& audio_;
    std::atomic<bool> playing_ { false };
    std::atomic<bool> stop_requested_ { false };
    TaskHandle_t task_ = nullptr;
    Source source_ = Source::None;
    std::string path_;
    const int16_t* pcm_samples_ = nullptr;
    size_t pcm_sample_count_ = 0;

    static void TaskEntry(void* arg);
    bool StartTask();
    void Run();
    void RunFile();
    void RunPcm();
};
