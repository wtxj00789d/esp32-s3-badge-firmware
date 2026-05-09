#include "badge_application.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "badge_defaults.h"

namespace {
constexpr const char* TAG = "BadgeApplication";

const char* ButtonEventName(BadgeButtonEvent event)
{
    switch (event) {
    case BadgeButtonEvent::SingleClick:
        return "single click";
    case BadgeButtonEvent::DoubleClick:
        return "double click";
    case BadgeButtonEvent::LongPress:
        return "long press";
    }
    return "unknown";
}
}

void BadgeApplication::Initialize()
{
    ESP_LOGI(TAG, "Initialize badge firmware");
    board_.Initialize();

    action_queue_ = xQueueCreate(8, sizeof(BadgeAction));
    if (action_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create badge action queue");
        state_ = BadgeState::ErrorNotice;
        board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::ErrorPage());
        return;
    }

    board_.SetButtonCallback([this](BadgeButtonEvent event) {
        HandleButton(event);
    });

    settings_.Open();
    bool sd_ok = storage_.Mount();

    if (!sd_ok) {
        ESP_LOGW(TAG, "SD storage unavailable; using fallback page");
        state_ = BadgeState::NoSdFallback;
        board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::DefaultPage());
    } else if (storage_.Media().empty()) {
        ESP_LOGW(TAG, "No badge media found on SD; using fallback page");
        state_ = BadgeState::NoSdFallback;
        board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::DefaultPage());
    } else {
        ESP_LOGI(TAG, "Badge media ready: %u item(s)", static_cast<unsigned>(storage_.Media().size()));
        SelectInitialWallpaper();
        if (LoadCurrentWallpaper()) {
            state_ = BadgeState::PlayingWallpaper;
            DrawWallpaperFrame();
        } else {
            state_ = BadgeState::NoSdFallback;
            board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::DefaultPage());
        }
    }
}

void BadgeApplication::Run()
{
    ESP_LOGI(TAG, "Run badge firmware");
    while (true) {
        if (action_queue_ == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        BadgeAction action;
        if (xQueueReceive(action_queue_, &action, pdMS_TO_TICKS(50)) == pdTRUE) {
            HandleAction(action);
        }

        if (state_ == BadgeState::PlayingWallpaper && current_bwp_.loaded()) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us >= next_frame_time_us_) {
                DrawWallpaperFrame();
            }
        }
    }
}

void BadgeApplication::HandleButton(BadgeButtonEvent event)
{
    ESP_LOGI(TAG, "BOOT button: %s", ButtonEventName(event));

    BadgeAction action;
    bool should_queue = true;

    if (state_ == BadgeState::Recording) {
        switch (event) {
        case BadgeButtonEvent::SingleClick:
            action = BadgeAction::StopRecording;
            break;
        case BadgeButtonEvent::DoubleClick:
        case BadgeButtonEvent::LongPress:
            should_queue = false;
            break;
        }
    } else {
        switch (event) {
        case BadgeButtonEvent::SingleClick:
            action = BadgeAction::PlaySound;
            break;
        case BadgeButtonEvent::DoubleClick:
            action = BadgeAction::NextWallpaper;
            break;
        case BadgeButtonEvent::LongPress:
            action = BadgeAction::StartRecording;
            break;
        }
    }

    if (!should_queue) {
        return;
    }

    if (action_queue_ == nullptr) {
        ESP_LOGW(TAG, "Badge action queue is not available");
        return;
    }

    if (xQueueSend(action_queue_, &action, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Badge action queue is full");
    }
}

void BadgeApplication::HandleAction(BadgeAction action)
{
    switch (action) {
    case BadgeAction::PlaySound:
        ESP_LOGI(TAG, "Action: play sound");
        break;
    case BadgeAction::NextWallpaper:
        ESP_LOGI(TAG, "Action: next wallpaper");
        AdvanceWallpaper();
        break;
    case BadgeAction::StartRecording:
        ESP_LOGI(TAG, "Action: start recording");
        state_ = BadgeState::Recording;
        board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::RecordingPage());
        break;
    case BadgeAction::StopRecording:
        ESP_LOGI(TAG, "Action: stop recording");
        if (current_bwp_.loaded()) {
            state_ = BadgeState::PlayingWallpaper;
            DrawWallpaperFrame();
        } else {
            state_ = BadgeState::NoSdFallback;
            board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::DefaultPage());
        }
        break;
    }
}

bool BadgeApplication::LoadCurrentWallpaper()
{
    const auto& media = storage_.Media();
    if (!storage_.mounted() || media.empty() || current_media_index_ >= media.size()) {
        current_bwp_.Close();
        return false;
    }

    if (!current_bwp_.Load(media[current_media_index_].bwp_path.c_str())) {
        current_frame_index_ = 0;
        next_frame_time_us_ = 0;
        return false;
    }

    current_frame_index_ = 0;
    next_frame_time_us_ = esp_timer_get_time();
    return true;
}

void BadgeApplication::DrawWallpaperFrame()
{
    if (!current_bwp_.loaded()) {
        return;
    }

    const uint16_t* frame = current_bwp_.Frame(current_frame_index_);
    if (frame == nullptr) {
        current_frame_index_ = 0;
        frame = current_bwp_.Frame(current_frame_index_);
    }

    if (frame == nullptr) {
        return;
    }

    board_.DrawRgb565(0, 0, current_bwp_.width(), current_bwp_.height(), frame);
    current_frame_index_ = (current_frame_index_ + 1) % current_bwp_.frame_count();
    next_frame_time_us_ = esp_timer_get_time() + 1000000LL / current_bwp_.fps();
}

void BadgeApplication::SelectInitialWallpaper()
{
    current_media_index_ = 0;

    const std::string current_basename = settings_.CurrentBasename();
    if (current_basename.empty()) {
        return;
    }

    const auto& media = storage_.Media();
    for (size_t i = 0; i < media.size(); ++i) {
        if (media[i].basename == current_basename) {
            current_media_index_ = i;
            return;
        }
    }
}

void BadgeApplication::AdvanceWallpaper()
{
    const auto& media = storage_.Media();
    if (!storage_.mounted() || media.empty()) {
        current_bwp_.Close();
        state_ = BadgeState::NoSdFallback;
        board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::DefaultPage());
        return;
    }

    const size_t start_index = current_media_index_;
    for (size_t offset = 1; offset <= media.size(); ++offset) {
        current_media_index_ = (start_index + offset) % media.size();
        if (LoadCurrentWallpaper()) {
            settings_.SetCurrentBasename(media[current_media_index_].basename);
            state_ = BadgeState::PlayingWallpaper;
            DrawWallpaperFrame();
            return;
        }
    }

    current_bwp_.Close();
    state_ = BadgeState::NoSdFallback;
    board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::DefaultPage());
}
