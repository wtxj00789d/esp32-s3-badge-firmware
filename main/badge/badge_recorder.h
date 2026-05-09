#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio_codec.h"

class BadgeRecorder {
public:
    explicit BadgeRecorder(AudioCodec& audio);
    ~BadgeRecorder();

    bool Start(const char* path);
    void Stop();
    bool recording() const { return recording_; }

private:
    AudioCodec& audio_;
    std::atomic<bool> recording_ { false };
    std::atomic<bool> stop_requested_ { false };
    TaskHandle_t task_ = nullptr;
    FILE* file_ = nullptr;
    uint32_t bytes_written_ = 0;

    static void TaskEntry(void* arg);
    void Run();
    bool WritePlaceholderHeader();
    void FinalizeHeader();
};
