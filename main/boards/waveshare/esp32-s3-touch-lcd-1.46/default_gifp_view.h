#ifndef DEFAULT_GIFP_VIEW_H_
#define DEFAULT_GIFP_VIEW_H_

#include "device_state.h"
#include "gifp_player.h"

#include <esp_timer.h>

class DefaultGifpView {
public:
    DefaultGifpView();
    ~DefaultGifpView();

    void Show();
    void Hide();
    void SetState(DeviceState state);
    bool visible() const { return visible_; }

private:
    GifpPlayer player_;
    DeviceState state_ = kDeviceStateIdle;
    esp_timer_handle_t idle_timer_ = nullptr;
    bool visible_ = false;
    int idle_ticks_ = 0;

    void PlayForState(DeviceState state);
    void PlayIdleSurprise();
    static void OnIdleTimer(void* arg);
};

#endif
