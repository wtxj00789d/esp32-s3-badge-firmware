# Badge Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Xiaozhi application path with a pure electronic badge firmware for the Spotpear ESP32-S3 1.28 inch round LCD box.

**Architecture:** Build a new `main/badge` application layer and a badge-specific board wrapper that reuse proven low-level LCD, ES8311, button, backlight, NVS, and SDMMC code. Do not initialize LVGL, Wi-Fi, OTA, wake word, chat protocols, or Xiaozhi state machines.

**Tech Stack:** ESP-IDF C++ firmware, FreeRTOS tasks/events, ESP LCD GC9A01 panel driver, ES8311 audio codec, SDMMC/FATFS, NVS, `iot_button`.

---

## File Structure

Create badge-specific files:

```text
main/badge/badge_application.h
main/badge/badge_application.cc
main/badge/badge_board.h
main/badge/badge_board.cc
main/badge/badge_defaults.h
main/badge/badge_defaults.cc
main/badge/badge_input.h
main/badge/badge_input.cc
main/badge/badge_storage.h
main/badge/badge_storage.cc
main/badge/badge_settings.h
main/badge/badge_settings.cc
main/badge/badge_bwp.h
main/badge/badge_bwp.cc
main/badge/badge_sound.h
main/badge/badge_sound.cc
main/badge/badge_recorder.h
main/badge/badge_recorder.cc
```

Modify existing files:

```text
main/main.cc
main/CMakeLists.txt
main/idf_component.yml
main/boards/sp-esp32-s3-1.28-box/config.h
.gitignore
```

Keep as low-level sources for reuse:

```text
main/audio/audio_codec.*
main/audio/codecs/es8311_audio_codec.*
main/boards/common/backlight.*
main/boards/common/button.*
main/boards/common/i2c_device.*
main/boards/common/system_reset.*
main/led/single_led.*
```

Do not compile these into the badge firmware:

```text
main/application.*
main/protocols/*
main/mcp_server.*
main/ota.*
main/device_state*
main/audio/audio_service.*
main/audio/processors/*
main/audio/wake_words/*
main/display/lvgl_display/*
main/display/emote_display.*
main/display/oled_display.*
```

## Task 1: CMake Switch To Pure Badge Firmware

**Files:**
- Modify: `main/CMakeLists.txt`
- Modify: `main/idf_component.yml`
- Modify: `main/main.cc`
- Create: `main/badge/badge_application.h`
- Create: `main/badge/badge_application.cc`

- [ ] **Step 1: Replace source list with badge-focused files**

In `main/CMakeLists.txt`, replace the initial `set(SOURCES ...)` block with this minimal badge list:

```cmake
set(SOURCES
    "main.cc"
    "badge/badge_application.cc"
    "badge/badge_board.cc"
    "badge/badge_defaults.cc"
    "badge/badge_input.cc"
    "badge/badge_storage.cc"
    "badge/badge_settings.cc"
    "badge/badge_bwp.cc"
    "badge/badge_sound.cc"
    "badge/badge_recorder.cc"
    "audio/audio_codec.cc"
    "audio/codecs/es8311_audio_codec.cc"
    "boards/common/backlight.cc"
    "boards/common/button.cc"
    "boards/common/i2c_device.cc"
    "boards/common/system_reset.cc"
    "led/single_led.cc"
)

set(INCLUDE_DIRS
    "."
    "badge"
    "audio"
    "audio/codecs"
    "boards/common"
    "boards/sp-esp32-s3-1.28-box"
    "led"
)
```

Remove board auto-selection code from `main/CMakeLists.txt` for the first badge firmware. The only board target is `sp-esp32-s3-1.28-box`.

- [ ] **Step 2: Keep component registration direct**

At the bottom of `main/CMakeLists.txt`, make sure `idf_component_register` uses only the new variables:

```cmake
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
)
```

If the file has asset embedding, LVGL font generation, emoji generation, or manufacturer board selection blocks below this point, remove those blocks from the compiled badge path.

- [ ] **Step 3: Trim component dependencies**

Open `main/idf_component.yml`. Keep dependencies required by LCD, codec, button, and filesystem. Remove dependencies used only by protocols, cloud, wake word, LVGL UI, or OTA after confirming they are not referenced by the badge sources.

The first dependency set should include these kinds of components:

```yaml
dependencies:
  espressif/button: "*"
  espressif/esp_codec_dev: "*"
  espressif/esp_lcd_gc9a01: "*"
  idf: ">=5.3"
```

If the existing file already pins compatible component versions, preserve the version pins for these kept components.

- [ ] **Step 4: Replace app entrypoint**

Replace `main/main.cc` with:

```cpp
#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include "badge_application.h"

namespace {
constexpr const char* TAG = "main";
}

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to recover from version mismatch");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    BadgeApplication app;
    app.Initialize();
    app.Run();
}
```

- [ ] **Step 5: Add temporary BadgeApplication skeleton**

Create `main/badge/badge_application.h`:

```cpp
#pragma once

class BadgeApplication {
public:
    void Initialize();
    void Run();
};
```

Create `main/badge/badge_application.cc`:

```cpp
#include "badge_application.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr const char* TAG = "BadgeApplication";
}

void BadgeApplication::Initialize()
{
    ESP_LOGI(TAG, "Initialize badge firmware");
}

void BadgeApplication::Run()
{
    ESP_LOGI(TAG, "Run badge firmware");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

- [ ] **Step 6: Build to expose missing dependencies**

Run:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$env:IDF_PATH='C:\Espressif\frameworks\esp-idf-v5.5.2'
$env:IDF_TOOLS_PATH='C:\Espressif'
$py='C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'
$activate = & $py C:\Espressif\frameworks\esp-idf-v5.5.2\tools\activate.py --export | Select-Object -First 1
. $activate
& $py C:\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py build
```

Expected: The build either passes with a tiny idle badge firmware or fails only on missing component dependencies caused by the trimmed `idf_component.yml`.

- [ ] **Step 7: Fix dependency-only build failures**

If the build fails on missing headers for `button`, `esp_codec_dev`, `esp_lcd_gc9a01`, or `driver` APIs, add the required dependency to `main/idf_component.yml` and rerun the build. Do not re-add LVGL, protocols, OTA, or audio service sources.

- [ ] **Step 8: Commit**

```powershell
git add main/CMakeLists.txt main/idf_component.yml main/main.cc main/badge/badge_application.h main/badge/badge_application.cc
git commit -m "build: switch to badge firmware entrypoint"
```

## Task 2: BadgeBoard Direct Hardware Bring-Up

**Files:**
- Create: `main/badge/badge_board.h`
- Create: `main/badge/badge_board.cc`
- Modify: `main/badge/badge_application.h`
- Modify: `main/badge/badge_application.cc`

- [ ] **Step 1: Define board interface**

Create `main/badge/badge_board.h`:

```cpp
#pragma once

#include <cstdint>
#include <functional>

#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include "audio_codec.h"

enum class BadgeButtonEvent {
    SingleClick,
    DoubleClick,
    LongPress,
};

class BadgeBoard {
public:
    using ButtonCallback = std::function<void(BadgeButtonEvent)>;

    BadgeBoard();
    ~BadgeBoard();

    void Initialize();
    void SetButtonCallback(ButtonCallback callback);
    void DrawRgb565(const uint16_t* pixels, int x, int y, int width, int height);
    void SetBacklightPercent(int percent);

    AudioCodec& Audio();
    int Width() const { return 240; }
    int Height() const { return 240; }

private:
    void InitializeCodecI2c();
    void InitializeDisplaySpi();
    void InitializeDisplayPanel();
    void InitializeButton();

    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t lcd_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    AudioCodec* audio_ = nullptr;
    void* boot_button_ = nullptr;
    ButtonCallback button_callback_;
};
```

- [ ] **Step 2: Implement display and audio setup**

Create `main/badge/badge_board.cc` by copying only the direct hardware setup ideas from `main/boards/sp-esp32-s3-1.28-box/sp-esp32-s3-1.28-box.cc`. Include:

```cpp
#include "badge_board.h"

#include <algorithm>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_lcd_gc9a01.h>
#include <esp_lcd_panel_vendor.h>

#include "button.h"
#include "config.h"
#include "es8311_audio_codec.h"

namespace {
constexpr const char* TAG = "BadgeBoard";
constexpr int kBacklightChannel = 0;
constexpr int kBacklightTimer = 0;
}

BadgeBoard::BadgeBoard() = default;

BadgeBoard::~BadgeBoard()
{
    delete static_cast<Button*>(boot_button_);
    delete audio_;
    if (panel_) {
        esp_lcd_panel_del(panel_);
    }
    if (lcd_io_) {
        esp_lcd_panel_io_del(lcd_io_);
    }
    if (codec_i2c_bus_) {
        i2c_del_master_bus(codec_i2c_bus_);
    }
}

void BadgeBoard::Initialize()
{
    InitializeCodecI2c();
    InitializeDisplaySpi();
    InitializeDisplayPanel();
    InitializeButton();
    audio_ = new Es8311AudioCodec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
        AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
    audio_->Start();
    SetBacklightPercent(80);
}
```

Complete the helper methods with the same pins and GC9A01 commands used by the existing board file. Keep the LCD path direct; do not create `CustomLcdDisplay`, `SpiLcdDisplay`, or LVGL objects.

- [ ] **Step 3: Wire BOOT callbacks**

In `BadgeBoard::InitializeButton()`, use the existing `Button` helper:

```cpp
void BadgeBoard::InitializeButton()
{
    auto* button = new Button(BOOT_BUTTON_GPIO);
    button->OnClick([this]() {
        if (button_callback_) {
            button_callback_(BadgeButtonEvent::SingleClick);
        }
    });
    button->OnDoubleClick([this]() {
        if (button_callback_) {
            button_callback_(BadgeButtonEvent::DoubleClick);
        }
    });
    button->OnLongPress([this]() {
        if (button_callback_) {
            button_callback_(BadgeButtonEvent::LongPress);
        }
    });
    boot_button_ = button;
}
```

- [ ] **Step 4: Add direct draw method**

```cpp
void BadgeBoard::DrawRgb565(const uint16_t* pixels, int x, int y, int width, int height)
{
    if (!panel_ || !pixels) {
        return;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_, x, y, x + width, y + height, pixels));
}
```

- [ ] **Step 5: Use board from application**

Change `BadgeApplication` to own `BadgeBoard board_;`, initialize it, and draw a solid color frame in `Initialize()`:

```cpp
#include <vector>

void BadgeApplication::Initialize()
{
    board_.Initialize();
    std::vector<uint16_t> frame(board_.Width() * board_.Height(), 0x001F);
    board_.DrawRgb565(frame.data(), 0, 0, board_.Width(), board_.Height());
}
```

- [ ] **Step 6: Build and flash**

Run the build command from Task 1, then flash:

```powershell
& $py C:\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py -p COM7 flash monitor
```

Expected: The board boots into the badge firmware and shows a solid blue screen. BOOT presses log badge button events once they are wired in Task 3 or Task 4.

- [ ] **Step 7: Commit**

```powershell
git add main/badge/badge_board.* main/badge/badge_application.*
git commit -m "feat: bring up badge board hardware"
```

## Task 3: Defaults And Direct-Draw Status Screens

**Files:**
- Create: `main/badge/badge_defaults.h`
- Create: `main/badge/badge_defaults.cc`
- Modify: `main/badge/badge_application.cc`

- [ ] **Step 1: Add generated default frames**

Create `main/badge/badge_defaults.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace badge_defaults {

constexpr int kWidth = 240;
constexpr int kHeight = 240;
constexpr int kPixelCount = kWidth * kHeight;

const uint16_t* DefaultPage();
const uint16_t* RecordingPage();
const uint16_t* ErrorPage();

}  // namespace badge_defaults
```

Create `main/badge/badge_defaults.cc` with simple generated RGB565 images. Use compact runtime initialization rather than storing three full arrays:

```cpp
#include "badge_defaults.h"

#include <array>

namespace badge_defaults {
namespace {

std::array<uint16_t, kPixelCount> MakePage(uint16_t bg, uint16_t ring, uint16_t center)
{
    std::array<uint16_t, kPixelCount> pixels {};
    const int cx = kWidth / 2;
    const int cy = kHeight / 2;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const int dx = x - cx;
            const int dy = y - cy;
            const int d2 = dx * dx + dy * dy;
            uint16_t color = bg;
            if (d2 < 82 * 82 && d2 > 66 * 66) {
                color = ring;
            }
            if (d2 < 38 * 38) {
                color = center;
            }
            pixels[y * kWidth + x] = color;
        }
    }
    return pixels;
}

const auto kDefault = MakePage(0x0000, 0x07E0, 0xFFFF);
const auto kRecording = MakePage(0x0000, 0xF800, 0xFFFF);
const auto kError = MakePage(0x0000, 0xFFE0, 0xF800);

}  // namespace

const uint16_t* DefaultPage() { return kDefault.data(); }
const uint16_t* RecordingPage() { return kRecording.data(); }
const uint16_t* ErrorPage() { return kError.data(); }

}  // namespace badge_defaults
```

- [ ] **Step 2: Draw default page on boot**

In `BadgeApplication::Initialize()`, replace the solid color frame with:

```cpp
board_.DrawRgb565(badge_defaults::DefaultPage(), 0, 0, board_.Width(), board_.Height());
```

- [ ] **Step 3: Build and flash**

Expected: The screen shows the generated default page, with no LVGL UI elements, status bar, text, or Xiaozhi face.

- [ ] **Step 4: Commit**

```powershell
git add main/badge/badge_defaults.* main/badge/badge_application.cc
git commit -m "feat: add badge default screens"
```

## Task 4: Input And Application State Machine

**Files:**
- Create: `main/badge/badge_input.h`
- Create: `main/badge/badge_input.cc`
- Modify: `main/badge/badge_application.h`
- Modify: `main/badge/badge_application.cc`

- [ ] **Step 1: Define badge states and queued actions**

In `badge_application.h`, add:

```cpp
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
```

Add private members:

```cpp
BadgeBoard board_;
BadgeState state_ = BadgeState::Booting;
QueueHandle_t action_queue_ = nullptr;

void HandleButton(BadgeButtonEvent event);
void HandleAction(BadgeAction action);
```

- [ ] **Step 2: Create action queue**

In `Initialize()`:

```cpp
action_queue_ = xQueueCreate(8, sizeof(BadgeAction));
board_.SetButtonCallback([this](BadgeButtonEvent event) {
    HandleButton(event);
});
```

- [ ] **Step 3: Map button events by state**

Implement:

```cpp
void BadgeApplication::HandleButton(BadgeButtonEvent event)
{
    BadgeAction action;
    if (state_ == BadgeState::Recording) {
        if (event != BadgeButtonEvent::SingleClick) {
            return;
        }
        action = BadgeAction::StopRecording;
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
    xQueueSend(action_queue_, &action, 0);
}
```

- [ ] **Step 4: Process actions in Run**

Replace the idle loop with:

```cpp
void BadgeApplication::Run()
{
    BadgeAction action;
    while (true) {
        if (xQueueReceive(action_queue_, &action, pdMS_TO_TICKS(50)) == pdTRUE) {
            HandleAction(action);
        }
    }
}
```

Make `HandleAction()` log the action and change screens for recording start/stop:

```cpp
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
        board_.DrawRgb565(badge_defaults::RecordingPage(), 0, 0, board_.Width(), board_.Height());
        break;
    case BadgeAction::StopRecording:
        ESP_LOGI(TAG, "Action: stop recording");
        state_ = BadgeState::PlayingWallpaper;
        board_.DrawRgb565(badge_defaults::DefaultPage(), 0, 0, board_.Width(), board_.Height());
        break;
    }
}
```

- [ ] **Step 5: Build and test BOOT behavior**

Flash and monitor. Expected logs:

```text
single click -> Action: play sound
double click -> Action: next wallpaper
long press -> Action: start recording
single click while recording -> Action: stop recording
```

- [ ] **Step 6: Commit**

```powershell
git add main/badge/badge_application.* main/badge/badge_input.*
git commit -m "feat: add badge input state machine"
```

## Task 5: SD Storage And NVS Settings

**Files:**
- Create: `main/badge/badge_storage.h`
- Create: `main/badge/badge_storage.cc`
- Create: `main/badge/badge_settings.h`
- Create: `main/badge/badge_settings.cc`
- Modify: `main/badge/badge_application.*`

- [ ] **Step 1: Define storage media model**

Create `badge_storage.h`:

```cpp
#pragma once

#include <string>
#include <vector>

struct BadgeMediaItem {
    std::string basename;
    std::string bwp_path;
    std::string wav_path;
    bool has_wav = false;
};

class BadgeStorage {
public:
    bool Mount();
    bool mounted() const { return mounted_; }
    const std::vector<BadgeMediaItem>& Media() const { return media_; }
    std::string NextRecordingPath(int number) const;

private:
    bool EnsureDirectory(const char* path);
    void ScanMedia();
    bool FileExists(const std::string& path) const;

    bool mounted_ = false;
    std::vector<BadgeMediaItem> media_;
};
```

- [ ] **Step 2: Implement SDMMC mount**

In `badge_storage.cc`, use the confirmed pins:

```cpp
constexpr gpio_num_t kSdClk = GPIO_NUM_17;
constexpr gpio_num_t kSdCmd = GPIO_NUM_18;
constexpr gpio_num_t kSdD0 = GPIO_NUM_21;
constexpr gpio_num_t kSdD3 = GPIO_NUM_13;
constexpr const char* kMountPoint = "/sdcard";
constexpr const char* kBadgeDir = "/sdcard/BADGE";
constexpr const char* kRecDir = "/sdcard/REC";
```

Mount with `esp_vfs_fat_sdmmc_mount`, `SDMMC_HOST_DEFAULT()`, `SDMMC_SLOT_CONFIG_DEFAULT()`, `slot_config.width = 1`, and the GPIOs above.

- [ ] **Step 3: Scan 8.3 BWP files**

In `ScanMedia()`, open `/sdcard/BADGE`, accept filenames matching exactly `NNN.BWP`, and derive `NNN.WAV`. Sort by basename.

Use `dirent` and `stat`, not string parsing of raw directory buffers.

- [ ] **Step 4: Add settings wrapper**

Create `badge_settings.h`:

```cpp
#pragma once

#include <string>

class BadgeSettings {
public:
    bool Open();
    std::string CurrentBasename() const;
    void SetCurrentBasename(const std::string& basename);
    int NextRecordingNumber();

private:
    bool opened_ = false;
};
```

Implement with NVS namespace `badge`, keys `current` and `rec_next`. `NextRecordingNumber()` returns the current integer and stores the next integer, starting at `1`.

- [ ] **Step 5: Wire storage into boot**

In `BadgeApplication::Initialize()`:

```cpp
settings_.Open();
const bool sd_ok = storage_.Mount();
if (!sd_ok) {
    state_ = BadgeState::NoSdFallback;
    board_.DrawRgb565(badge_defaults::DefaultPage(), 0, 0, board_.Width(), board_.Height());
    return;
}
state_ = storage_.Media().empty() ? BadgeState::NoSdFallback : BadgeState::PlayingWallpaper;
```

- [ ] **Step 6: Build and flash with empty SD**

Expected:

```text
/sdcard/BADGE exists
/sdcard/REC exists
empty media list is accepted
default page remains visible
recording remains allowed if /sdcard/REC is writable
```

- [ ] **Step 7: Commit**

```powershell
git add main/badge/badge_storage.* main/badge/badge_settings.* main/badge/badge_application.*
git commit -m "feat: mount badge sd storage"
```

## Task 6: BWP Parser And Wallpaper Playback

**Files:**
- Create: `main/badge/badge_bwp.h`
- Create: `main/badge/badge_bwp.cc`
- Modify: `main/badge/badge_application.*`

- [ ] **Step 1: Define BWP v1 header**

Use this first firmware format:

```cpp
#pragma pack(push, 1)
struct BwpHeader {
    char magic[4];          // "BWP1"
    uint16_t width;         // 240
    uint16_t height;        // 240
    uint16_t fps;           // 1..15
    uint16_t frame_count;   // >= 1
    uint32_t frame_bytes;   // width * height * 2
};
#pragma pack(pop)
```

Frame payload is raw RGB565 frames, stored consecutively after the header.

- [ ] **Step 2: Implement loader**

Create `BadgeBwp` with:

```cpp
class BadgeBwp {
public:
    bool Load(const char* path);
    void Close();
    bool loaded() const;
    int width() const;
    int height() const;
    int fps() const;
    int frame_count() const;
    const uint16_t* Frame(int index) const;
};
```

The first implementation reads the whole BWP into PSRAM using `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`. Reject files when:

```text
magic != BWP1
width != 240
height != 240
fps < 1
fps > 15
frame_count < 1
frame_bytes != 240 * 240 * 2
payload size != frame_count * frame_bytes
```

- [ ] **Step 3: Add playback timer loop**

In `BadgeApplication`, add current media index, current BWP, and frame index. In `Run()`, if state is `PlayingWallpaper`, draw the next frame every `1000 / fps` ms. Use `esp_timer_get_time()` for frame timing.

- [ ] **Step 4: Implement double-click next wallpaper**

On `BadgeAction::NextWallpaper`:

```text
if storage media list is empty -> redraw default page
else stop current BWP, advance index, load next valid BWP, save basename to NVS
```

If a BWP fails to load, skip it and try the next one. If all fail, draw the default page.

- [ ] **Step 5: Build and test with `001.BWP`**

Put a valid raw BWP on SD:

```text
/sdcard/BADGE/001.BWP
```

Expected: It displays at its declared fps. Double-click cycles valid BWP files.

- [ ] **Step 6: Commit**

```powershell
git add main/badge/badge_bwp.* main/badge/badge_application.*
git commit -m "feat: play bwp wallpapers"
```

## Task 7: WAV Sound Playback

**Files:**
- Create: `main/badge/badge_sound.h`
- Create: `main/badge/badge_sound.cc`
- Modify: `main/badge/badge_application.*`

- [ ] **Step 1: Define WAV parser**

Support only:

```text
RIFF WAVE
fmt chunk PCM
channels = 1
sample_rate = 24000
bits_per_sample = 16
data chunk present
```

Reject unsupported files with an `ESP_LOGW`, not an assert.

- [ ] **Step 2: Implement playback worker**

Create `BadgeSoundPlayer`:

```cpp
class BadgeSoundPlayer {
public:
    explicit BadgeSoundPlayer(AudioCodec& audio);
    bool Play(const char* path);
    void Stop();
    bool playing() const;
};
```

`Play()` opens the file, validates header, enables output, reads PCM in chunks of 240 samples, and calls `AudioCodec::OutputData(std::vector<int16_t>& data)`. Keep playback synchronous for the first version; the app can ignore new play requests while sound is playing.

- [ ] **Step 3: Wire single-click**

On `BadgeAction::PlaySound`, find the current media item's `wav_path`. If `has_wav` is false, log and return. Keep the current wallpaper visible.

- [ ] **Step 4: Test**

Place:

```text
/sdcard/BADGE/001.BWP
/sdcard/BADGE/001.WAV
```

Expected: Single-click plays the WAV through ES8311 speaker. Missing WAV does not crash or switch page.

- [ ] **Step 5: Commit**

```powershell
git add main/badge/badge_sound.* main/badge/badge_application.*
git commit -m "feat: play wallpaper wav sounds"
```

## Task 8: WAV Recorder

**Files:**
- Create: `main/badge/badge_recorder.h`
- Create: `main/badge/badge_recorder.cc`
- Modify: `main/badge/badge_application.*`

- [ ] **Step 1: Define recorder interface**

```cpp
class BadgeRecorder {
public:
    explicit BadgeRecorder(AudioCodec& audio);
    bool Start(const char* path);
    void Stop();
    bool recording() const;

private:
    static void TaskEntry(void* arg);
    void TaskMain();
};
```

- [ ] **Step 2: Write WAV header helpers**

At start, write a 44-byte PCM WAV header with `data_size = 0`. On stop, seek to byte 4 and byte 40 and update RIFF size and data size.

Use:

```text
sample_rate = 24000
channels = 1
bits_per_sample = 16
byte_rate = 48000
block_align = 2
```

- [ ] **Step 3: Stream microphone input**

`TaskMain()` should:

```text
enable audio input
read 240-sample chunks with AudioCodec::InputData
write raw int16_t PCM to FILE*
count bytes written
flush periodically
stop when requested
disable input
repair WAV header
close file
```

- [ ] **Step 4: Wire long-press start and recording single-click stop**

On `StartRecording`:

```text
stop BWP playback loop
draw RecordingPage
ask BadgeSettings for next recording number
format /sdcard/REC/REC0001.WAV
start recorder
state = Recording only if Start() succeeds
```

On `StopRecording`:

```text
recorder.Stop()
state = PlayingWallpaper or NoSdFallback
reload/draw current wallpaper
```

- [ ] **Step 5: Test 10-second recording**

Expected:

```text
/sdcard/REC/REC0001.WAV exists
file size is about 480000 bytes plus 44-byte header for 10 seconds
PC can open WAV
screen returns to current wallpaper after stop
```

- [ ] **Step 6: Test 60-second recording**

Expected:

```text
file size is about 2880000 bytes plus 44-byte header
no SD write underrun errors
wallpaper animation is paused during recording
```

- [ ] **Step 7: Commit**

```powershell
git add main/badge/badge_recorder.* main/badge/badge_application.*
git commit -m "feat: record wav files to sd"
```

## Task 9: Remove Old Xiaozhi Compile Surface

**Files:**
- Modify: `main/CMakeLists.txt`
- Optionally delete unused files after compile proof

- [ ] **Step 1: Confirm old sources are not compiled**

Run:

```powershell
& $py C:\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py build
```

Then inspect build commands or generated object files:

```powershell
Get-ChildItem -Recurse build\esp-idf\main -Filter *.obj | Select-String -Pattern "application|protocol|lvgl|ota|wake"
```

Expected: No object file from old Xiaozhi application, protocol, OTA, wake word, or LVGL display stack.

- [ ] **Step 2: Delete badge-irrelevant source directories only after build passes**

Remove old application source files from git tracking if the badge build no longer references them:

```powershell
git rm main/application.cc main/application.h
git rm -r main/protocols
git rm main/mcp_server.cc main/mcp_server.h
git rm main/ota.cc main/ota.h
git rm main/device_state.cc main/device_state.h main/device_state_machine.cc main/device_state_machine.h
git rm -r main/audio/processors main/audio/wake_words
git rm -r main/display/lvgl_display
git rm main/display/emote_display.cc main/display/emote_display.h
git rm main/display/oled_display.cc main/display/oled_display.h
```

If a listed file does not exist, omit that exact path and continue. Do not delete reused low-level files.

- [ ] **Step 3: Build again**

Expected: Badge firmware still builds. If a deleted file was still needed, restore only that file with `git restore --staged <path>` and `git restore <path>`, then replace that dependency with a badge-local helper in a later small commit.

- [ ] **Step 4: Commit**

```powershell
git add main/CMakeLists.txt
git commit -m "refactor: remove old xiaozhi app surface"
```

## Task 10: End-To-End Hardware Verification

**Files:**
- Modify: `docs/superpowers/specs/2026-05-09-badge-firmware-design.md` only if measured limits change

- [ ] **Step 1: Verify no-SD boot**

Remove SD and flash firmware. Expected: default page appears, button actions do not crash, recording is rejected with an error page or log.

- [ ] **Step 2: Verify empty-SD boot**

Insert empty SD. Expected:

```text
/sdcard/BADGE
/sdcard/REC
```

are created, default page appears, recording is allowed.

- [ ] **Step 3: Verify media playback**

Put:

```text
/sdcard/BADGE/001.BWP
/sdcard/BADGE/001.WAV
/sdcard/BADGE/002.BWP
```

Expected:

```text
boot shows 001.BWP or last selected BWP
single-click plays 001.WAV
double-click switches to 002.BWP
reboot preserves selected wallpaper
```

- [ ] **Step 4: Verify recording**

Long-press to record for 10 seconds, single-click to stop. Expected: `/sdcard/REC/REC0001.WAV` exists and opens on PC.

- [ ] **Step 5: Verify long recording**

Record for 60 seconds. Expected: file size about 2.88 MB, no reboot, no SD write errors, screen returns to wallpaper.

- [ ] **Step 6: Commit verification notes**

If measured behavior changes the design constraints, update the design doc and commit:

```powershell
git add docs/superpowers/specs/2026-05-09-badge-firmware-design.md
git commit -m "docs: record badge firmware verification results"
```

If the design constraints remain unchanged, do not make a docs-only commit.

## Self-Review

Spec coverage:

- Pure badge firmware: Tasks 1 and 9.
- No LVGL: Tasks 1, 2, 9.
- Direct LCD drawing: Tasks 2 and 3.
- SD storage: Task 5.
- BWP playback: Task 6.
- WAV sound playback: Task 7.
- BOOT button state machine: Task 4.
- WAV recording: Task 8.
- Hardware verification: Task 10.

Placeholder scan:

- This plan avoids deferred placeholder sections and gives exact file paths, commands, expected behavior, and first-version formats.

Type consistency:

- `BadgeButtonEvent`, `BadgeState`, `BadgeAction`, `BadgeBoard`, `BadgeStorage`, `BadgeSettings`, `BadgeBwp`, `BadgeSoundPlayer`, and `BadgeRecorder` are introduced before use in implementation tasks.

