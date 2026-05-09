#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <memory>

#include "badge_board.h"
#include "badge_bwp.h"
#include "badge_recorder.h"
#include "badge_settings.h"
#include "badge_sound.h"
#include "badge_storage.h"

enum class BadgeState {
    Booting,
    NoSdFallback,
    PlayingWallpaper,
    PlayingSound,
    Recording,
    ErrorNotice,
};

enum class BadgeAction {
    PlaySound,
    NextWallpaper,
    StartRecording,
    StopRecording,
};

class BadgeApplication {
public:
    void Initialize();
    void Run();

private:
    BadgeBoard board_;
    BadgeStorage storage_;
    BadgeSettings settings_;
    BadgeBwp current_bwp_;
    std::unique_ptr<BadgeSoundPlayer> sound_player_;
    std::unique_ptr<BadgeRecorder> recorder_;
    size_t current_media_index_ = 0;
    int current_frame_index_ = 0;
    int64_t next_frame_time_us_ = 0;
    BadgeState state_ = BadgeState::Booting;
    QueueHandle_t action_queue_ = nullptr;

    void HandleButton(BadgeButtonEvent event);
    void HandleAction(BadgeAction action);
    bool LoadCurrentWallpaper();
    void DrawWallpaperFrame();
    void SelectInitialWallpaper();
    void AdvanceWallpaper();
    void PlayCurrentSound();
    void StartRecording();
    void StopRecording();
    void ResumeDisplayAfterRecording();
};
