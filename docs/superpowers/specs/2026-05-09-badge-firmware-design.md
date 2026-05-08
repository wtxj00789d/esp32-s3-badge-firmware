# Badge Firmware Design

Date: 2026-05-09
Target board: Spotpear ESP32-S3 1.28 inch round LCD box (`sp-esp32-s3-1.28-box`)

## Goal

Build a dedicated electronic badge firmware with:

- boot-time wallpaper playback
- BOOT button sound playback
- BOOT button wallpaper switching
- BOOT button audio recording
- SD card storage for wallpapers, sounds, and recordings

This is a pure badge firmware. It does not keep the Xiaozhi chat application, network protocol flow, activation flow, OTA flow, wake word flow, or Wi-Fi configuration flow. The project may reuse proven low-level source code from the existing Xiaozhi tree, especially board initialization, LCD panel setup, audio codec setup, button helpers, backlight helpers, and NVS helpers.

## Non-Goals For First Firmware

- No LVGL.
- No Xiaozhi application mode.
- No chat state machine.
- No Wi-Fi, MQTT, WebSocket, MCP, OTA, activation, or wake-word logic.
- No touch UI.
- No Android app implementation in this phase.
- No BLE file transfer implementation in this phase.
- No GIF runtime decoding.
- No long filename requirement on SD.

The Android app and BLE file service will be designed after the standalone badge firmware is stable. The firmware should keep clean storage and file operation boundaries so BLE transfer can be added later without rewriting playback and recording logic.

## Hardware Facts

Confirmed board facts:

- ESP32-S3, 16 MB flash, 8 MB Octal PSRAM.
- LCD: GC9A01, 240x240, RGB565, SPI.
- Audio codec: ES8311.
- BOOT button: GPIO0.
- SD card works in SDMMC 1-bit mode.

Relevant pins:

```text
LCD SCLK   GPIO4
LCD MOSI   GPIO2
LCD CS     GPIO5
LCD DC     GPIO47
LCD RESET  GPIO38

Audio MCLK GPIO16
Audio WS   GPIO45
Audio BCLK GPIO9
Audio DIN  GPIO10
Audio DOUT GPIO8
Audio PA   GPIO46
Audio SDA  GPIO15
Audio SCL  GPIO14

BOOT       GPIO0

SD CLK     GPIO17
SD CMD     GPIO18
SD D0      GPIO21
SD D3      GPIO13
```

SD probe result:

```text
SD mode: SDMMC 1-bit
Card size: 486 MB
Sequential write speed: about 314.2 KiB/s
60 second recording-rate write test: passed
Test payload: 2,880,000 bytes at about 48 KB/s
```

This is enough for 24 kHz, 16-bit, mono PCM WAV recording. It is not enough to safely combine recording writes with continuous high-bandwidth animation reads, so dynamic wallpaper playback must pause during recording.

## Architecture

The first firmware should replace the original Xiaozhi application path with a dedicated badge path:

```text
app_main
  |
  +-- nvs_flash_init
  |
  +-- BadgeBoard
  |     +-- LCD panel init, no LVGL
  |     +-- ES8311 audio codec init
  |     +-- BOOT button init
  |     +-- backlight and optional power helpers
  |     +-- SDMMC pin config
  |
  +-- BadgeApplication
        +-- BadgeStorage
        +-- BadgeBwpPlayer
        +-- BadgeSoundPlayer
        +-- BadgeRecorder
        +-- BadgeInput
        +-- BadgeSettings
        +-- BadgeDefaults
```

The display path should write RGB565 frame buffers directly to the `esp_lcd_panel_handle_t` through `esp_lcd_panel_draw_bitmap`. LVGL should not be initialized or linked into the first badge firmware.

## Components

### BadgeBoard

Owns board-level hardware setup and exposes simple handles to the application:

- LCD panel handle
- display size and orientation
- audio codec object
- BOOT button events
- backlight control
- SD card pins and mount helper

It should reuse existing board initialization code where useful, but it should be a badge-specific board implementation rather than a `WifiBoard`.

### BadgeApplication

Owns the product state machine:

```text
Booting
  -> NoSdFallback
  -> PlayingWallpaper
  -> PlayingSound
  -> Recording
  -> ErrorNotice
```

The application is the only component that decides when playback, sound, and recording may start or stop.

### BadgeStorage

Mounts SD, creates required directories, scans media files, and exposes a validated media list.

Required directories:

```text
/sdcard/BADGE
/sdcard/REC
```

First firmware should use short 8.3 filenames:

```text
/sdcard/BADGE/001.BWP
/sdcard/BADGE/001.WAV
/sdcard/BADGE/002.BWP
/sdcard/BADGE/002.WAV
/sdcard/REC/REC0001.WAV
```

Long filename support can be enabled and tested later, but the first firmware should not depend on it.

### BadgeSettings

Persists small settings in NVS:

- current wallpaper basename or index
- volume if needed
- next recording number

If settings are missing or invalid, the app falls back to the first valid SD wallpaper. If SD has no valid wallpaper, it uses the embedded default page.

### BadgeBwpPlayer

Plays `.BWP` wallpapers.

First BWP target:

```text
resolution: 240x240
format: RGB565
target fps: up to 15
source: SD card or embedded default
```

The first implementation should prefer loading the current BWP into PSRAM before playback. If the file is too large, malformed, or unsupported, it should be rejected and the app should fall back to the embedded default page.

Static images are represented as one-frame BWP files.

### BadgeSoundPlayer

Plays the current wallpaper's same-basename WAV file:

```text
001.BWP -> 001.WAV
```

First format:

```text
WAV PCM
24000 Hz preferred
16-bit
mono
```

The player may reject unsupported WAV formats in the first firmware. Missing WAV files are not fatal; the app should either play the embedded default sound or do nothing.

### BadgeRecorder

Records microphone input to SD as WAV:

```text
WAV PCM
24000 Hz
16-bit
mono
about 48 KB/s
```

Recording behavior:

- Stop BWP animation before recording starts.
- Show a lightweight direct-draw recording screen.
- Write a placeholder WAV header at start.
- Stream PCM data to SD.
- On normal stop, seek back and fix the WAV header.
- Return to the selected wallpaper after stop.

If power is lost during recording, the file may have an incomplete header in the first firmware. Header repair can be added later.

### BadgeInput

BOOT button behavior:

```text
normal:
  single click  -> play current sound
  double click  -> switch to next wallpaper
  long press    -> start recording

recording:
  single click  -> stop recording
  double click  -> ignored
  long press    -> ignored
```

The original BOOT behaviors from Xiaozhi are removed. BOOT no longer enters Wi-Fi configuration and no longer toggles chat.

## Data Flow

Boot flow:

```text
boot
  |
  +-- initialize hardware
  +-- mount SD
  +-- scan /sdcard/BADGE
  +-- load last selected wallpaper from NVS
  +-- validate selected wallpaper
  +-- display selected wallpaper or embedded default
```

Sound flow:

```text
single click
  |
  +-- find same-basename WAV
  +-- pause/coordinate audio output if needed
  +-- play WAV
  +-- keep wallpaper visible
```

Wallpaper switch flow:

```text
double click
  |
  +-- stop current BWP playback
  +-- select next valid BWP
  +-- persist selection
  +-- start new BWP playback
```

Recording flow:

```text
long press
  |
  +-- stop BWP playback
  +-- open next RECxxxx.WAV
  +-- show recording screen
  +-- stream PCM to SD
  |
single click while recording
  |
  +-- stop PCM stream
  +-- repair WAV header
  +-- return to wallpaper
```

## Error Handling

SD mount failure:

- Use embedded default page.
- Use embedded default sound or silence.
- Disable wallpaper switching.
- Disable recording.

Empty SD:

- Create `/sdcard/BADGE` and `/sdcard/REC`.
- Use embedded default page.
- Disable wallpaper switching until media exists.
- Allow recording if `/sdcard/REC` is writable.

Invalid BWP:

- Skip it during scan if possible.
- If the selected BWP becomes invalid, select the next valid BWP.
- If no valid BWP exists, use embedded default page.

Missing WAV:

- Keep the wallpaper valid.
- Play default sound or remain silent.

Recording open/write failure:

- Stop recording immediately.
- Close the file if opened.
- Show a lightweight error screen briefly.
- Return to wallpaper playback.

## Build Shape

The firmware should remove or stop compiling the old Xiaozhi application modules that are not needed. The exact CMake cleanup should be done carefully after dependency inspection.

Expected kept areas:

```text
board-specific LCD/audio/button/backlight helpers
ES8311 audio codec
direct esp_lcd GC9A01 setup
NVS/settings helper if lightweight
SDMMC/FATFS support
```

Expected removed or unlinked areas:

```text
LVGL display stack
protocols
MCP server
OTA
Wi-Fi app flow
MQTT/WebSocket
wake words
AFE/VAD processors
activation
chat state machine
locale/UI assets
GIF runtime display code
```

## Testing Plan

Hardware verification should proceed in this order:

1. Build and flash pure badge firmware.
2. Confirm boot display shows embedded default page without SD.
3. Confirm SD mounts and directories are created.
4. Confirm `001.BWP` displays from SD.
5. Confirm double click switches wallpapers and persists across reboot.
6. Confirm single click plays `001.WAV`.
7. Confirm long press starts recording and single click stops recording.
8. Confirm recording file appears under `/sdcard/REC`.
9. Confirm recording WAV can be read from the SD card on PC.
10. Confirm recording mode pauses BWP animation and returns to wallpaper afterward.

## First Implementation Boundary

The first implementation is considered successful when:

- The firmware boots directly into badge mode.
- The old Xiaozhi application behavior is absent.
- LVGL is not initialized or required.
- The default page works without SD.
- SD BWP playback works.
- Same-basename WAV playback works.
- BOOT single/double/long behavior matches the badge design.
- PCM WAV recording to SD works and produces PC-readable files.

