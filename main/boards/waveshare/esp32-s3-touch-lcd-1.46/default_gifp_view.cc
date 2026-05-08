#include "default_gifp_view.h"
#include "default_ui_gifp_assets.h"

#include <cstdlib>

#include <esp_random.h>
#include <esp_log.h>
#include <lvgl.h>

#define TAG "DefaultGifpView"

DefaultGifpView::DefaultGifpView() {
    esp_timer_create_args_t args = {
        .callback = DefaultGifpView::OnIdleTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "default_gifp_idle",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &idle_timer_));
}

DefaultGifpView::~DefaultGifpView() {
    Hide();
    if (idle_timer_ != nullptr) {
        esp_timer_delete(idle_timer_);
        idle_timer_ = nullptr;
    }
}

void DefaultGifpView::Show() {
    ESP_LOGI(TAG, "Show default GIFP view");
    visible_ = true;
    idle_ticks_ = 0;
    PlayForState(state_);
    if (idle_timer_ != nullptr) {
        esp_timer_stop(idle_timer_);
        ESP_ERROR_CHECK(esp_timer_start_periodic(idle_timer_, 10 * 1000 * 1000));
    }
}

void DefaultGifpView::Hide() {
    ESP_LOGI(TAG, "Hide default GIFP view");
    visible_ = false;
    if (idle_timer_ != nullptr) {
        esp_timer_stop(idle_timer_);
    }
    player_.Stop();
}

void DefaultGifpView::SetState(DeviceState state) {
    if (state_ == state && visible_) {
        return;
    }
    state_ = state;
    idle_ticks_ = 0;
    if (visible_) {
        PlayForState(state_);
    }
}

void DefaultGifpView::PlayForState(DeviceState state) {
    if (state == kDeviceStateListening) {
        ESP_LOGI(TAG, "Play listening GIFP, size=%u", static_cast<unsigned>(kDefaultUiListeningGifpSize));
        player_.StartMemory(kDefaultUiListeningGifp, kDefaultUiListeningGifpSize);
    } else if (state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Play speaking GIFP, size=%u", static_cast<unsigned>(kDefaultUiSpeakingGifpSize));
        player_.StartMemory(kDefaultUiSpeakingGifp, kDefaultUiSpeakingGifpSize);
    } else if (state == kDeviceStateConnecting || state == kDeviceStateActivating) {
        ESP_LOGI(TAG, "Play thinking GIFP, size=%u", static_cast<unsigned>(kDefaultUiThinkingGifpSize));
        player_.StartMemory(kDefaultUiThinkingGifp, kDefaultUiThinkingGifpSize);
    } else {
        ESP_LOGI(TAG, "Play idle GIFP, size=%u", static_cast<unsigned>(kDefaultUiIdleGifpSize));
        player_.StartMemory(kDefaultUiIdleGifp, kDefaultUiIdleGifpSize);
    }
}

void DefaultGifpView::PlayIdleSurprise() {
    if (!visible_ || state_ != kDeviceStateIdle) {
        return;
    }
    if ((esp_random() & 1) == 0) {
        player_.StartMemory(kDefaultUiDroolingFaceGifp, kDefaultUiDroolingFaceGifpSize);
    } else {
        player_.StartMemory(kDefaultUiSleepingFaceGifp, kDefaultUiSleepingFaceGifpSize);
    }
}

void DefaultGifpView::OnIdleTimer(void* arg) {
    auto* view = static_cast<DefaultGifpView*>(arg);
    if (!view->visible_ || view->state_ != kDeviceStateIdle) {
        return;
    }
    view->idle_ticks_++;
    if (view->idle_ticks_ >= 6) {
        view->idle_ticks_ = 0;
        view->PlayIdleSurprise();
    }
}
