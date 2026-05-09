#include "badge_board.h"

#include <algorithm>

#include <esp_check.h>
#include <esp_codec_dev_defaults.h>
#include <esp_err.h>
#include <esp_lcd_gc9a01.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "button.h"
#include "es8311_audio_codec.h"

namespace {
constexpr const char* TAG = "BadgeBoard";

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;
constexpr int kDisplaySpiClockHz = 40 * 1000 * 1000;

constexpr gpio_num_t kLcdSclk = GPIO_NUM_4;
constexpr gpio_num_t kLcdMosi = GPIO_NUM_2;
constexpr gpio_num_t kLcdCs = GPIO_NUM_5;
constexpr gpio_num_t kLcdDc = GPIO_NUM_47;
constexpr gpio_num_t kLcdReset = GPIO_NUM_38;
constexpr gpio_num_t kBacklight = GPIO_NUM_42;
constexpr bool kBacklightInvert = true;

constexpr gpio_num_t kAudioMclk = GPIO_NUM_16;
constexpr gpio_num_t kAudioWs = GPIO_NUM_45;
constexpr gpio_num_t kAudioBclk = GPIO_NUM_9;
constexpr gpio_num_t kAudioDin = GPIO_NUM_10;
constexpr gpio_num_t kAudioDout = GPIO_NUM_8;
constexpr gpio_num_t kAudioPa = GPIO_NUM_46;
constexpr gpio_num_t kAudioI2cSda = GPIO_NUM_15;
constexpr gpio_num_t kAudioI2cScl = GPIO_NUM_14;
constexpr int kAudioSampleRate = 24000;

constexpr gpio_num_t kBootButton = GPIO_NUM_0;

constexpr ledc_mode_t kBacklightLedcMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBacklightLedcTimer = LEDC_TIMER_1;
constexpr ledc_channel_t kBacklightLedcChannel = LEDC_CHANNEL_1;
constexpr uint32_t kBacklightMaxDuty = 1023;
}

BadgeBoard::~BadgeBoard()
{
    delete boot_button_;
    boot_button_ = nullptr;

    delete audio_;
    audio_ = nullptr;

    if (backlight_initialized_) {
        ledc_stop(kBacklightLedcMode, kBacklightLedcChannel, 0);
        backlight_initialized_ = false;
    }

    if (panel_) {
        esp_lcd_panel_del(panel_);
        panel_ = nullptr;
    }

    if (panel_io_) {
        esp_lcd_panel_io_del(panel_io_);
        panel_io_ = nullptr;
    }

    if (codec_i2c_bus_) {
        i2c_del_master_bus(codec_i2c_bus_);
        codec_i2c_bus_ = nullptr;
    }
}

void BadgeBoard::Initialize()
{
    InitializeCodecI2c();
    InitializeDisplaySpi();
    InitializeDisplayPanel();
    InitializeBacklight();
    InitializeAudio();
    InitializeButton();
    SetBacklightPercent(80);
}

void BadgeBoard::SetButtonCallback(std::function<void(BadgeButtonEvent)> callback)
{
    button_callback_ = std::move(callback);
}

void BadgeBoard::DrawRgb565(int x, int y, int width, int height, const uint16_t* pixels)
{
    if (!panel_ || !pixels || width <= 0 || height <= 0) {
        return;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_, x, y, x + width, y + height, pixels));
}

void BadgeBoard::SetBacklightPercent(uint8_t percent)
{
    if (!backlight_initialized_) {
        return;
    }

    percent = std::min<uint8_t>(percent, 100);
    const uint32_t duty = (kBacklightMaxDuty * percent) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(kBacklightLedcMode, kBacklightLedcChannel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(kBacklightLedcMode, kBacklightLedcChannel));
}

AudioCodec& BadgeBoard::Audio()
{
    return *audio_;
}

int BadgeBoard::Width() const
{
    return kDisplayWidth;
}

int BadgeBoard::Height() const
{
    return kDisplayHeight;
}

void BadgeBoard::InitializeCodecI2c()
{
    ESP_LOGI(TAG, "Initialize ES8311 I2C bus");
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = kAudioI2cSda,
        .scl_io_num = kAudioI2cScl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
}

void BadgeBoard::InitializeDisplaySpi()
{
    ESP_LOGI(TAG, "Initialize GC9A01 SPI bus");
    spi_bus_config_t bus_config = GC9A01_PANEL_BUS_SPI_CONFIG(
        kLcdSclk,
        kLcdMosi,
        kDisplayWidth * kDisplayHeight * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO));
}

void BadgeBoard::InitializeDisplayPanel()
{
    ESP_LOGI(TAG, "Initialize GC9A01 panel");
    esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(kLcdCs, kLcdDc, nullptr, nullptr);
    io_config.pclk_hz = kDisplaySpiClockHz;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = kLcdReset;
    panel_config.rgb_endian = LCD_RGB_ENDIAN_BGR;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io_, &panel_config, &panel_));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    const uint8_t data_0x62[] = {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_, 0x62, data_0x62, sizeof(data_0x62)));

    const uint8_t data_0x63[] = {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_, 0x63, data_0x63, sizeof(data_0x63)));

    const uint8_t data_0x36[] = {0x48};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_, 0x36, data_0x36, sizeof(data_0x36)));

    const uint8_t data_0xC3[] = {0x1F};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_, 0xC3, data_0xC3, sizeof(data_0xC3)));

    const uint8_t data_0xC4[] = {0x1F};
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io_, 0xC4, data_0xC4, sizeof(data_0xC4)));
}

void BadgeBoard::InitializeBacklight()
{
    ESP_LOGI(TAG, "Initialize LCD backlight PWM");
    const ledc_timer_config_t timer_config = {
        .speed_mode = kBacklightLedcMode,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = kBacklightLedcTimer,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    const ledc_channel_config_t channel_config = {
        .gpio_num = kBacklight,
        .speed_mode = kBacklightLedcMode,
        .channel = kBacklightLedcChannel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = kBacklightLedcTimer,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = kBacklightInvert,
        },
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
    backlight_initialized_ = true;
}

void BadgeBoard::InitializeAudio()
{
    ESP_LOGI(TAG, "Initialize ES8311 audio codec");
    audio_ = new Es8311AudioCodec(
        codec_i2c_bus_,
        I2C_NUM_0,
        kAudioSampleRate,
        kAudioSampleRate,
        kAudioMclk,
        kAudioBclk,
        kAudioWs,
        kAudioDout,
        kAudioDin,
        kAudioPa,
        ES8311_CODEC_DEFAULT_ADDR);
}

void BadgeBoard::InitializeButton()
{
    ESP_LOGI(TAG, "Initialize BOOT button");
    boot_button_ = new Button(kBootButton);
    boot_button_->OnClick([this]() {
        EmitButtonEvent(BadgeButtonEvent::SingleClick);
    });
    boot_button_->OnDoubleClick([this]() {
        EmitButtonEvent(BadgeButtonEvent::DoubleClick);
    });
    boot_button_->OnLongPress([this]() {
        EmitButtonEvent(BadgeButtonEvent::LongPress);
    });
}

void BadgeBoard::EmitButtonEvent(BadgeButtonEvent event)
{
    if (button_callback_) {
        button_callback_(event);
    }
}
