#include "badge_application.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "badge_defaults.h"
#include "badge_default_assets.h"

namespace {
constexpr const char* TAG = "BadgeApplication";
constexpr int64_t kIdleQueueWaitUs = 50 * 1000;
constexpr int64_t kStartupButtonIgnoreUs = 3 * 1000 * 1000;
constexpr const char* kEmbeddedWallpaperSettingsValue = "__default__";

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
    board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::LoadingPage());
    ignore_button_until_us_ = esp_timer_get_time() + kStartupButtonIgnoreUs;
    sound_player_ = std::make_unique<BadgeSoundPlayer>(board_.Audio());
    recorder_ = std::make_unique<BadgeRecorder>(board_.Audio());

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
        StartEmbeddedWallpaper();
    } else if (storage_.Media().empty()) {
        ESP_LOGW(TAG, "No badge media found on SD; using fallback page");
        StartEmbeddedWallpaper();
    } else {
        ESP_LOGI(TAG, "Badge media ready: %u item(s)", static_cast<unsigned>(storage_.Media().size()));
        SelectInitialWallpaper();
        if (use_embedded_wallpaper_) {
            StartEmbeddedWallpaper();
            return;
        }

        const auto& media = storage_.Media();
        const size_t start_index = current_media_index_;
        bool loaded = false;
        for (size_t offset = 0; offset < media.size(); ++offset) {
            current_media_index_ = (start_index + offset) % media.size();
            if (LoadCurrentWallpaper()) {
                settings_.SetCurrentBasename(media[current_media_index_].basename);
                state_ = BadgeState::PlayingWallpaper;
                DrawWallpaperFrame();
                loaded = true;
                break;
            }
        }

        if (!loaded) {
            current_bwp_.Close();
            StartEmbeddedWallpaper();
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

        int64_t wait_us = kIdleQueueWaitUs;
        if (state_ == BadgeState::PlayingWallpaper && current_bwp_.loaded()) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us >= next_frame_time_us_) {
                wait_us = 0;
            } else {
                wait_us = next_frame_time_us_ - now_us;
                if (wait_us > kIdleQueueWaitUs) {
                    wait_us = kIdleQueueWaitUs;
                }
            }
        } else if (state_ == BadgeState::PlayingEmbeddedWallpaper) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us >= embedded_next_frame_time_us_) {
                wait_us = 0;
            } else {
                wait_us = embedded_next_frame_time_us_ - now_us;
                if (wait_us > kIdleQueueWaitUs) {
                    wait_us = kIdleQueueWaitUs;
                }
            }
        }

        BadgeAction action;
        if (xQueueReceive(action_queue_, &action, pdMS_TO_TICKS((wait_us + 999) / 1000)) == pdTRUE) {
            HandleAction(action);
        }

        if (state_ == BadgeState::PlayingWallpaper && current_bwp_.loaded()) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us >= next_frame_time_us_) {
                DrawWallpaperFrame();
            }
        } else if (state_ == BadgeState::PlayingEmbeddedWallpaper) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us >= embedded_next_frame_time_us_) {
                DrawEmbeddedWallpaperFrame();
            }
        }
    }
}

void BadgeApplication::HandleButton(BadgeButtonEvent event)
{
    if (esp_timer_get_time() < ignore_button_until_us_) {
        ESP_LOGI(TAG, "Ignore startup BOOT button event: %s", ButtonEventName(event));
        return;
    }

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
        PlayCurrentSound();
        break;
    case BadgeAction::NextWallpaper:
        ESP_LOGI(TAG, "Action: next wallpaper");
        AdvanceWallpaper();
        break;
    case BadgeAction::StartRecording:
        ESP_LOGI(TAG, "Action: start recording");
        StartRecording();
        break;
    case BadgeAction::StopRecording:
        ESP_LOGI(TAG, "Action: stop recording");
        StopRecording();
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
    use_embedded_wallpaper_ = false;

    const std::string current_basename = settings_.CurrentBasename();
    if (current_basename.empty()) {
        return;
    }

    if (current_basename == kEmbeddedWallpaperSettingsValue) {
        use_embedded_wallpaper_ = true;
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
        StartEmbeddedWallpaper();
        return;
    }

    if (state_ == BadgeState::PlayingEmbeddedWallpaper) {
        current_media_index_ = 0;
        if (LoadCurrentWallpaper()) {
            settings_.SetCurrentBasename(media[current_media_index_].basename);
            state_ = BadgeState::PlayingWallpaper;
            DrawWallpaperFrame();
            return;
        }
    }

    if (state_ == BadgeState::PlayingWallpaper && current_media_index_ + 1 >= media.size()) {
        current_bwp_.Close();
        settings_.SetCurrentBasename(kEmbeddedWallpaperSettingsValue);
        StartEmbeddedWallpaper();
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
    StartEmbeddedWallpaper();
}

void BadgeApplication::PlayCurrentSound()
{
    if (state_ == BadgeState::PlayingEmbeddedWallpaper) {
        PlayDefaultSound();
        return;
    }

    const auto& media = storage_.Media();
    if (!storage_.mounted() || media.empty() || current_media_index_ >= media.size()) {
        ESP_LOGW(TAG, "No SD media sound available");
        PlayDefaultSound();
        return;
    }

    const BadgeMediaItem& item = media[current_media_index_];
    if (!item.has_wav) {
        ESP_LOGW(TAG, "No matching WAV for %s", item.basename.c_str());
        PlayDefaultSound();
        return;
    }

    if (!sound_player_) {
        ESP_LOGW(TAG, "Sound player is not ready");
        return;
    }

    sound_player_->Play(item.wav_path.c_str());
}

void BadgeApplication::PlayDefaultSound()
{
    if (!sound_player_) {
        ESP_LOGW(TAG, "Sound player is not ready");
        return;
    }
    sound_player_->PlayPcm(badge_default_assets::SoundPcm(), badge_default_assets::SoundSampleCount());
}

void BadgeApplication::StartEmbeddedWallpaper()
{
    current_bwp_.Close();
    state_ = BadgeState::PlayingEmbeddedWallpaper;
    embedded_frame_index_ = 0;
    embedded_next_frame_time_us_ = esp_timer_get_time();
    DrawEmbeddedWallpaperFrame();
}

void BadgeApplication::DrawEmbeddedWallpaperFrame()
{
    if (embedded_frame_buffer_ == nullptr) {
        embedded_frame_buffer_ = static_cast<uint16_t*>(heap_caps_malloc(
            badge_default_assets::kWidth * badge_default_assets::kHeight * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (embedded_frame_buffer_ == nullptr) {
            embedded_frame_buffer_ = static_cast<uint16_t*>(heap_caps_malloc(
                badge_default_assets::kWidth * badge_default_assets::kHeight * sizeof(uint16_t),
                MALLOC_CAP_8BIT));
        }
    }

    if (embedded_frame_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate embedded wallpaper frame buffer");
        state_ = BadgeState::NoSdFallback;
        board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::DefaultPage());
        return;
    }

    if (!badge_default_assets::DecodeWallpaperFrame(
            embedded_frame_index_,
            embedded_frame_buffer_,
            badge_default_assets::kWidth * badge_default_assets::kHeight)) {
        ESP_LOGW(TAG, "Failed to decode embedded wallpaper frame %d", embedded_frame_index_);
        embedded_frame_index_ = 0;
        return;
    }

    board_.DrawRgb565(0, 0, badge_default_assets::kWidth, badge_default_assets::kHeight, embedded_frame_buffer_);
    embedded_frame_index_ = (embedded_frame_index_ + 1) % badge_default_assets::kFrameCount;
    embedded_next_frame_time_us_ = esp_timer_get_time() + 1000000LL / badge_default_assets::kFps;
}

void BadgeApplication::StartRecording()
{
    if (!storage_.mounted()) {
        ESP_LOGW(TAG, "Cannot record without SD storage");
        state_ = BadgeState::NoSdFallback;
        StartEmbeddedWallpaper();
        return;
    }

    if (!recorder_) {
        ESP_LOGW(TAG, "Recorder is not ready");
        return;
    }

    if (sound_player_ && sound_player_->playing()) {
        sound_player_->Stop();
    }

    const int recording_number = settings_.NextRecordingNumber();
    const std::string path = storage_.NextRecordingPath(recording_number);
    if (!recorder_->Start(path.c_str())) {
        state_ = BadgeState::ErrorNotice;
        board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::ErrorPage());
        return;
    }

    state_ = BadgeState::Recording;
    board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::RecordingPage());
}

void BadgeApplication::StopRecording()
{
    if (recorder_) {
        recorder_->Stop();
    }
    ResumeDisplayAfterRecording();
}

void BadgeApplication::ResumeDisplayAfterRecording()
{
    if (current_bwp_.loaded()) {
        state_ = BadgeState::PlayingWallpaper;
        next_frame_time_us_ = esp_timer_get_time();
        DrawWallpaperFrame();
    } else {
        StartEmbeddedWallpaper();
    }
}
