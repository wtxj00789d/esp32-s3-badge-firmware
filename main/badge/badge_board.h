#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/i2c_master.h>

#include "audio_codec.h"

class Button;

enum class BadgeButtonEvent {
    SingleClick,
    DoubleClick,
    LongPress,
};

class BadgeBoard {
public:
    BadgeBoard() = default;
    ~BadgeBoard();

    void Initialize();
    void SetButtonCallback(std::function<void(BadgeButtonEvent)> callback);
    void DrawRgb565(int x, int y, int width, int height, const uint16_t* pixels);
    void SetBacklightPercent(uint8_t percent);
    AudioCodec& Audio();

    int Width() const;
    int Height() const;

private:
    void InitializeCodecI2c();
    void InitializeDisplaySpi();
    void InitializeDisplayPanel();
    void InitializeBacklight();
    void InitializeAudio();
    void InitializeButton();
    void EmitButtonEvent(BadgeButtonEvent event);
    bool EnsureLcdSwapBuffer(size_t pixel_count);

    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    uint16_t* lcd_swap_buffer_ = nullptr;
    size_t lcd_swap_pixels_ = 0;
    AudioCodec* audio_ = nullptr;
    Button* boot_button_ = nullptr;
    std::function<void(BadgeButtonEvent)> button_callback_;
    bool backlight_initialized_ = false;
};
