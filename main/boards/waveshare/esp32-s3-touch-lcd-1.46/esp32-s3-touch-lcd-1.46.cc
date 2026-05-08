#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "default_gifp_view.h"
#include "sd_wallpaper_manager.h"
#include "adc_battery_monitor.h"

#include <cstdio>
#include <esp_log.h>
#include "i2c_device.h"
#include <driver/adc_types_legacy.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_spd2010.h>
#include <esp_timer.h>
#include "esp_io_expander_tca9554.h"
#include "lcd_display.h"
#include <iot_button.h>

#define TAG "waveshare_lcd_1_46"

namespace {
constexpr uint16_t kBootLongPressMs = 2000;
constexpr uint16_t kBootMultiClickWindowMs = 420;
constexpr uint16_t kPowerLongPressMs = 5000;
}

// 在waveshare_lcd_1_46类之前添加新的显示类
class CustomLcdDisplay : public SpiLcdDisplay {
private:
    bool default_animation_mode_ = false;
    bool wallpaper_mode_ = false;

public:
    static void rounder_event_cb(lv_event_t * e) {
        lv_area_t * area = (lv_area_t *)lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        area->x1 = (x1 >> 2) << 2;          // round the start of coordinate down to the nearest 4M number
        area->x2 = ((x2 >> 2) << 2) + 3;    // round the end of coordinate up to the nearest 4N+3 number
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle, 
                    esp_lcd_panel_handle_t panel_handle,
                    int width,
                    int height,
                    int offset_x,
                    int offset_y,
                    bool mirror_x,
                    bool mirror_y,
                    bool swap_xy) 
        : SpiLcdDisplay(io_handle, panel_handle,
                    width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }

    virtual void SetEmotion(const char* emotion) override {
        if (wallpaper_mode_) {
            ESP_LOGD(TAG, "Suppress emotion while wallpaper mode is active: %s", emotion);
            return;
        }
        if (default_animation_mode_) {
            SetBuiltinEmojiVisible(false);
            ESP_LOGD(TAG, "Suppress builtin emotion while default GIFP view is active: %s", emotion);
            return;
        }
        SpiLcdDisplay::SetEmotion(emotion);
    }

    virtual void SetStatus(const char* status) override {
        if (wallpaper_mode_) {
            ESP_LOGD(TAG, "Suppress status while wallpaper mode is active: %s", status);
            return;
        }
        SpiLcdDisplay::SetStatus(status);
    }

    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override {
        if (wallpaper_mode_) {
            ESP_LOGI(TAG, "Suppress notification while wallpaper mode is active: %s", notification);
            return;
        }
        SpiLcdDisplay::ShowNotification(notification, duration_ms);
    }

    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000) override {
        ShowNotification(notification.c_str(), duration_ms);
    }

    virtual void SetChatMessage(const char* role, const char* content) override {
        if (wallpaper_mode_) {
            ESP_LOGD(TAG, "Suppress chat message while wallpaper mode is active");
            return;
        }
        SpiLcdDisplay::SetChatMessage(role, content);
    }

    virtual void ClearChatMessages() override {
        if (wallpaper_mode_) {
            ESP_LOGD(TAG, "Suppress clear chat messages while wallpaper mode is active");
            return;
        }
        SpiLcdDisplay::ClearChatMessages();
        if (default_animation_mode_) {
            SetBuiltinEmojiVisible(false);
        }
    }

    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override {
        if (wallpaper_mode_) {
            ESP_LOGD(TAG, "Suppress preview image while wallpaper mode is active");
            return;
        }
        SpiLcdDisplay::SetPreviewImage(std::move(image));
    }

    virtual void UpdateStatusBar(bool update_all = false) override {
        if (wallpaper_mode_) {
            ESP_LOGD(TAG, "Suppress status bar update while wallpaper mode is active");
            return;
        }
        SpiLcdDisplay::UpdateStatusBar(update_all);
    }

    void SetDefaultAnimationMode(bool enabled) {
        default_animation_mode_ = enabled;
        SetBuiltinEmojiVisible(!enabled);
    }

    void SetWallpaperMode(bool enabled) {
        wallpaper_mode_ = enabled;
        default_animation_mode_ = false;
        DisplayLockGuard lock(this);
        lv_obj_t* objects[] = {
            container_, top_bar_, status_bar_, bottom_bar_, emoji_box_,
            emoji_label_, emoji_image_, preview_image_, notification_label_
        };
        for (auto* object : objects) {
            if (object == nullptr) {
                continue;
            }
            if (enabled) {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    void SetBuiltinEmojiVisible(bool visible) {
        DisplayLockGuard lock(this);
        lv_obj_t* objects[] = {emoji_box_, emoji_label_, emoji_image_};
        for (auto* object : objects) {
            if (object == nullptr) {
                continue;
            }
            if (visible) {
                lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
};

class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    esp_io_expander_handle_t io_expander = NULL;
    LcdDisplay* display_;
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    button_handle_t boot_btn, pwr_btn;
    button_driver_t* boot_btn_driver_ = nullptr;
    button_driver_t* pwr_btn_driver_ = nullptr;
    DefaultGifpView default_view_;
    SdWallpaperManager* wallpaper_manager_ = nullptr;
    AdcBatteryMonitor* battery_monitor_ = nullptr;
    esp_timer_handle_t state_sync_timer_ = nullptr;
    esp_timer_handle_t startup_view_timer_ = nullptr;
    static CustomBoard* instance_;
    DeviceState last_default_view_state_ = kDeviceStateUnknown;

    void ShowDefaultPage() {
        if (wallpaper_manager_ != nullptr) {
            wallpaper_manager_->ShowDefaultPage();
        }
        static_cast<CustomLcdDisplay*>(display_)->SetWallpaperMode(false);
        static_cast<CustomLcdDisplay*>(display_)->SetDefaultAnimationMode(true);
        last_default_view_state_ = kDeviceStateUnknown;
        default_view_.Show();
        default_view_.SetState(Application::GetInstance().GetDeviceState());
    }

    void ShowBatteryStatus() {
        if (display_ == nullptr) {
            return;
        }
        int level = 0;
        bool charging = false;
        bool discharging = false;
        if (!GetBatteryLevel(level, charging, discharging)) {
            display_->ShowNotification("Battery status unavailable");
            return;
        }
        char message[32];
        snprintf(message, sizeof(message), "%s %d%%", charging ? "Charging" : "Battery", level);
        ESP_LOGI(TAG, "PWR single click: %s, discharging=%d", message, discharging);
        display_->ShowNotification(message);
    }

    static void OnStateSyncTimer(void* arg) {
        auto* self = static_cast<CustomBoard*>(arg);
        if (self->wallpaper_manager_ == nullptr || !self->wallpaper_manager_->wallpaper_mode()) {
            auto state = Application::GetInstance().GetDeviceState();
            static_cast<CustomLcdDisplay*>(self->display_)->SetDefaultAnimationMode(true);
            if (state != self->last_default_view_state_) {
                ESP_LOGI(TAG, "Default GIFP state sync: %d -> %d", self->last_default_view_state_, state);
                self->last_default_view_state_ = state;
            }
            self->default_view_.SetState(state);
        }
    }

    static void OnStartupViewTimer(void* arg) {
        auto* self = static_cast<CustomBoard*>(arg);
        if (self->wallpaper_manager_ == nullptr) {
            self->wallpaper_manager_ = new SdWallpaperManager(self->io_expander, self->panel_, self->panel_io_);
        }
        self->wallpaper_manager_->Initialize();
        if (self->wallpaper_manager_->RestoreSavedWallpaper()) {
            self->default_view_.Hide();
            static_cast<CustomLcdDisplay*>(self->display_)->SetWallpaperMode(true);
        } else {
            self->ShowDefaultPage();
        }
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = I2C_SDA_IO,
            .scl_io_num = I2C_SCL_IO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }
    
    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, I2C_ADDRESS, &io_expander);
        if(ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");        

        // uint32_t input_level_mask = 0;
        // ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, IO_EXPANDER_INPUT);               // 设置引脚 EXIO0 �?EXIO1 模式为输�?
        // ret = esp_io_expander_get_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, &input_level_mask);             // 获取引脚 EXIO0 �?EXIO1 的电平状�?存放�?input_level_mask �?
        // ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3, IO_EXPANDER_OUTPUT);              // 设置引脚 EXIO2 �?EXIO3 模式为输�?        // ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3, 1);                             // 将引脚电平设置为 1
        // ret = esp_io_expander_print_state(io_expander);                                                                             // 打印引脚状�?
        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, IO_EXPANDER_OUTPUT);                 // 设置引脚 EXIO0 �?EXIO1 模式为输�?        ESP_ERROR_CHECK(ret);
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);                                // 复位 LCD �?TouchPad
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 0);                                // 复位 LCD �?TouchPad
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);                                // 复位 LCD �?TouchPad
        ESP_ERROR_CHECK(ret);
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize QSPI bus");

        const spi_bus_config_t bus_config = TAIJIPI_SPD2010_PANEL_BUS_QSPI_CONFIG(QSPI_PIN_NUM_LCD_PCLK,
                                                                        QSPI_PIN_NUM_LCD_DATA0,
                                                                        QSPI_PIN_NUM_LCD_DATA1,
                                                                        QSPI_PIN_NUM_LCD_DATA2,
                                                                        QSPI_PIN_NUM_LCD_DATA3,
                                                                        QSPI_LCD_H_RES * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void InitializeSpd2010Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Install panel IO");
        
        const esp_lcd_panel_io_spi_config_t io_config = SPD2010_PANEL_IO_QSPI_CONFIG(QSPI_PIN_NUM_LCD_CS, NULL, NULL);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io));

        ESP_LOGI(TAG, "Install SPD2010 panel driver");
        
        spd2010_vendor_config_t vendor_config = {
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = QSPI_PIN_NUM_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,     // Implemented by LCD command `36h`
            .bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL,    // Implemented by LCD command `3Ah` (16/18)
            .vendor_config = &vendor_config,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_spd2010(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        panel_io_ = panel_io;
        panel_ = panel;
        display_ = new CustomLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }
 
    void InitializeButtonsCustom() {
        gpio_reset_pin(BOOT_BUTTON_GPIO);                                     
        gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);   
        gpio_reset_pin(PWR_BUTTON_GPIO);                                     
        gpio_set_direction(PWR_BUTTON_GPIO, GPIO_MODE_INPUT);   
        gpio_reset_pin(PWR_Control_PIN);                                     
        gpio_set_direction(PWR_Control_PIN, GPIO_MODE_OUTPUT);     
        // gpio_set_level(PWR_Control_PIN, false);
        gpio_set_level(PWR_Control_PIN, true);
    }

    void InitializeBatteryMonitor() {
        battery_monitor_ = new AdcBatteryMonitor(
            BATTERY_ADC_UNIT,
            BATTERY_ADC_CHANNEL,
            BATTERY_UPPER_RESISTOR,
            BATTERY_LOWER_RESISTOR,
            BATTERY_CHARGING_PIN);
    }

    void InitializeButtons() {
        instance_ = this;
        InitializeButtonsCustom();

        // Boot Button
        button_config_t boot_btn_config = {
            .long_press_time = kBootLongPressMs,
            .short_press_time = kBootMultiClickWindowMs
        };
        boot_btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        boot_btn_driver_->enable_power_save = false;
        boot_btn_driver_->get_key_level = [](button_driver_t *button_driver) -> uint8_t {
            return !gpio_get_level(BOOT_BUTTON_GPIO);
        };
        ESP_ERROR_CHECK(iot_button_create(&boot_btn_config, boot_btn_driver_, &boot_btn));
        iot_button_register_cb(boot_btn, BUTTON_SINGLE_CLICK, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<CustomBoard*>(usr_data);
            ESP_LOGI(TAG, "BOOT single click: switch wallpaper");
            Application::GetInstance().Schedule([self]() {
                if (self->wallpaper_manager_ == nullptr) {
                    self->wallpaper_manager_ = new SdWallpaperManager(self->io_expander, self->panel_, self->panel_io_);
                }
                if (self->wallpaper_manager_->SwitchToNext()) {
                    self->default_view_.Hide();
                    static_cast<CustomLcdDisplay*>(self->display_)->SetWallpaperMode(true);
                } else if (self->display_ != nullptr) {
                    self->display_->ShowNotification("No .gifp wallpaper on TF card");
                }
            });
        }, this);
        iot_button_register_cb(boot_btn, BUTTON_DOUBLE_CLICK, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<CustomBoard*>(usr_data);
            ESP_LOGI(TAG, "BOOT double click: show default page");
            Application::GetInstance().Schedule([self]() {
                self->ShowDefaultPage();
                if (self->display_ != nullptr) {
                    self->display_->ShowNotification("Default page");
                }
            });
        }, this);
        iot_button_register_cb(boot_btn, BUTTON_LONG_PRESS_START, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<CustomBoard*>(usr_data);
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                ESP_LOGI(TAG, "BOOT long press while starting: enter Wi-Fi config");
                self->EnterWifiConfigMode();
                return;
            }
            ESP_LOGI(TAG, "BOOT long press: toggle chat");
            app.ToggleChatState();
        }, this);

        // Power Button
        button_config_t pwr_btn_config = {
            .long_press_time = kPowerLongPressMs,
            .short_press_time = 0
        };
        pwr_btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        pwr_btn_driver_->enable_power_save = false;
        pwr_btn_driver_->get_key_level = [](button_driver_t *button_driver) -> uint8_t {
            return !gpio_get_level(PWR_BUTTON_GPIO);
        };
        ESP_ERROR_CHECK(iot_button_create(&pwr_btn_config, pwr_btn_driver_, &pwr_btn));
        iot_button_register_cb(pwr_btn, BUTTON_SINGLE_CLICK, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<CustomBoard*>(usr_data);
            Application::GetInstance().Schedule([self]() {
                self->ShowBatteryStatus();
            });
        }, this);
        iot_button_register_cb(pwr_btn, BUTTON_LONG_PRESS_START, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<CustomBoard*>(usr_data);
            if (self->GetBacklight()->brightness() > 0) {
                self->GetBacklight()->SetBrightness(0);
                gpio_set_level(PWR_Control_PIN, false);
            } else {
                self->GetBacklight()->RestoreBrightness();
                gpio_set_level(PWR_Control_PIN, true);
            }
        }, this);
    }

public:
    CustomBoard() { 
        InitializeI2c();
        InitializeTca9554();
        InitializeSpi();
        InitializeSpd2010Display();
        InitializeBatteryMonitor();
        InitializeButtons();
        esp_timer_create_args_t state_timer_args = {
            .callback = CustomBoard::OnStateSyncTimer,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "gifp_state_sync",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&state_timer_args, &state_sync_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(state_sync_timer_, 500 * 1000));
        esp_timer_create_args_t startup_timer_args = {
            .callback = CustomBoard::OnStartupViewTimer,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "gifp_startup_view",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&startup_timer_args, &startup_view_timer_));
        ESP_ERROR_CHECK(esp_timer_start_once(startup_view_timer_, 1000 * 1000));
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, I2S_STD_SLOT_LEFT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN, I2S_STD_SLOT_RIGHT); // I2S_STD_SLOT_LEFT / I2S_STD_SLOT_RIGHT / I2S_STD_SLOT_BOTH

        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        if (battery_monitor_ == nullptr) {
            return false;
        }
        charging = battery_monitor_->IsCharging();
        discharging = battery_monitor_->IsDischarging();
        level = battery_monitor_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(CustomBoard);

CustomBoard* CustomBoard::instance_ = nullptr;
