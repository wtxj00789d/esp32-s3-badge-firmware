#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "badge_board.h"

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
    BadgeState state_ = BadgeState::Booting;
    QueueHandle_t action_queue_ = nullptr;

    void HandleButton(BadgeButtonEvent event);
    void HandleAction(BadgeAction action);
};
