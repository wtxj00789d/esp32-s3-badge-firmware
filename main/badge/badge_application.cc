#include "badge_application.h"

#include <esp_log.h>
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

    board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::DefaultPage());
    if (!sd_ok) {
        ESP_LOGW(TAG, "SD storage unavailable; using fallback page");
        state_ = BadgeState::NoSdFallback;
    } else if (storage_.Media().empty()) {
        ESP_LOGW(TAG, "No badge media found on SD; using fallback page");
        state_ = BadgeState::NoSdFallback;
    } else {
        ESP_LOGI(TAG, "Badge media ready: %u item(s)", static_cast<unsigned>(storage_.Media().size()));
        state_ = BadgeState::PlayingWallpaper;
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
        break;
    case BadgeAction::StartRecording:
        ESP_LOGI(TAG, "Action: start recording");
        state_ = BadgeState::Recording;
        board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::RecordingPage());
        break;
    case BadgeAction::StopRecording:
        ESP_LOGI(TAG, "Action: stop recording");
        state_ = storage_.mounted() && !storage_.Media().empty()
            ? BadgeState::PlayingWallpaper
            : BadgeState::NoSdFallback;
        board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::DefaultPage());
        break;
    }
}
