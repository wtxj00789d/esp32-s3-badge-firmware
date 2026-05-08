# Waveshare 1.46 Wallpaper And Default Face Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add TF-card `.gifp` wallpaper switching, built-in default UI GIFP animations, and a Windows GUI/CLI converter for `.gifp` files.

**Architecture:** Use one shared GIFP codec/player format for both user wallpapers and built-in default UI animations. User wallpapers are `412x412` `.gifp` files read from TF card; default UI animations are `64x64` `.gifp` resources converted from `defaultUI/*.gif` and embedded in firmware, displayed centered on a black `412x412` screen.

**Tech Stack:** ESP-IDF/C++17, LVGL 9, esp-idf `fatfs`/SDSPI, TCA9554 IO expander, Python 3, Pillow, heatshrink2, tkinter, optional ffmpeg.

---

## File Structure

Firmware board-local files:

```text
main/boards/waveshare/esp32-s3-touch-lcd-1.46/
  gifp_player.h/.cc              GIFP validation, decoding, LVGL image playback
  default_gifp_view.h/.cc        Built-in 64x64 default UI animation selector
  sd_wallpaper_manager.h/.cc     TF card mount, scan, persistence, wallpaper switching
  heatshrink_*.h/.c              Decoder copied from H:\1.28inch_ESP32-2424S012
```

Windows/conversion files:

```text
tools/wallpaper_converter/
  app.py
  gifpack.py
  requirements.txt
  README.md
  tests/test_gifpack.py
```

Generated/default UI resources:

```text
main/boards/waveshare/esp32-s3-touch-lcd-1.46/default_ui/
  idle.gifp
  listening.gifp
  thinking.gifp
  speaking.gifp
  drooling_face.gifp
  sleeping_face.gifp
```

This folder is not a git repository. For commit steps, run `git rev-parse --is-inside-work-tree`; if it fails, record `commit skipped: not a git repository`.

---

### Task 1: Create The GIFP Converter Core

**Files:**
- Create: `tools/wallpaper_converter/requirements.txt`
- Create: `tools/wallpaper_converter/gifpack.py`
- Create: `tools/wallpaper_converter/tests/test_gifpack.py`

- [ ] **Step 1: Create requirements**

```text
Pillow>=10.0.0
heatshrink2>=0.13.0
```

- [ ] **Step 2: Copy encoder**

Copy `H:\1.28inch_ESP32-2424S012\gif_player_project\windows_app\gifpack.py` to `tools/wallpaper_converter/gifpack.py`.

Refactor constants so dimensions are parameters:

```python
DEFAULT_WIDTH = 412
DEFAULT_HEIGHT = 412

def geometry(width=DEFAULT_WIDTH, height=DEFAULT_HEIGHT):
    pixel_count = width * height
    return pixel_count, pixel_count * 2
```

Update public conversion/build APIs to accept `width` and `height`, defaulting to `412, 412`:

```python
def build_gifpack(frames, width=DEFAULT_WIDTH, height=DEFAULT_HEIGHT): ...
def parse_gifpack(blob): ...
def convert_gif_to_frames(path, crop=None, fps_cap=15, max_duration_ms=None, width=DEFAULT_WIDTH, height=DEFAULT_HEIGHT): ...
def convert_gif_to_gifpack(path, crop=None, fps_cap=15, max_duration_ms=None, width=DEFAULT_WIDTH, height=DEFAULT_HEIGHT, max_size=None): ...
```

Keep magic `GIFP`, version `1`, CRC32, RLE, delta, heatshrink, and indexed encodings compatible with the source project.

- [ ] **Step 3: Add tests**

Create `tools/wallpaper_converter/tests/test_gifpack.py`:

```python
import sys
import tempfile
import unittest
from pathlib import Path
from PIL import Image

APP_DIR = Path(__file__).resolve().parents[1]
if str(APP_DIR) not in sys.path:
    sys.path.insert(0, str(APP_DIR))

import gifpack


class GifpackTests(unittest.TestCase):
    def make_pixels(self, values):
        return b"".join(int(v).to_bytes(2, "little") for v in values)

    def test_rgb888_to_rgb565_little_endian(self):
        image = Image.new("RGB", (2, 1))
        image.putdata([(255, 0, 0), (0, 255, 0)])
        self.assertEqual(gifpack.rgb888_to_rgb565_bytes(image), b"\x00\xf8\xe0\x07")

    def test_build_parse_412(self):
        width = 412
        height = 412
        frame = gifpack.Frame(delay_ms=100, pixels=self.make_pixels([0x001F] * width * height))
        blob = gifpack.build_gifpack([frame], width=width, height=height)
        parsed = gifpack.parse_gifpack(blob)
        self.assertEqual(parsed.header.width, width)
        self.assertEqual(parsed.header.height, height)
        self.assertEqual(gifpack.decode_parsed_frames(parsed), [frame.pixels])

    def test_build_parse_64_for_default_ui(self):
        width = 64
        height = 64
        frame = gifpack.Frame(delay_ms=120, pixels=self.make_pixels([0x07E0] * width * height))
        blob = gifpack.build_gifpack([frame], width=width, height=height)
        parsed = gifpack.parse_gifpack(blob)
        self.assertEqual(parsed.header.width, width)
        self.assertEqual(parsed.header.height, height)
        self.assertEqual(gifpack.decode_parsed_frames(parsed), [frame.pixels])

    def test_crc_rejects_corruption(self):
        frame = gifpack.Frame(delay_ms=100, pixels=self.make_pixels([0xF800] * 64 * 64))
        blob = bytearray(gifpack.build_gifpack([frame], width=64, height=64))
        blob[-1] ^= 0x55
        with self.assertRaisesRegex(ValueError, "CRC"):
            gifpack.parse_gifpack(bytes(blob))

    def test_convert_default_ui_gif_to_64(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "idle.gif"
            a = Image.new("RGB", (64, 64), "black")
            b = Image.new("RGB", (64, 64), "cyan")
            a.save(path, save_all=True, append_images=[b], duration=[100, 100], loop=0)
            frames = gifpack.convert_gif_to_frames(str(path), fps_cap=10, max_duration_ms=1000, width=64, height=64)
        self.assertEqual(len(frames), 2)
        self.assertEqual(len(frames[0].pixels), 64 * 64 * 2)
```

- [ ] **Step 4: Run tests**

```powershell
python -m unittest discover -s tools\wallpaper_converter\tests -v
```

Expected: all tests pass.

---

### Task 2: Add GUI/CLI Converter And Generate Built-In Default UI GIFP

**Files:**
- Create: `tools/wallpaper_converter/app.py`
- Create: `tools/wallpaper_converter/README.md`
- Create: `main/boards/waveshare/esp32-s3-touch-lcd-1.46/default_ui/*.gifp`

- [ ] **Step 1: Implement app.py**

Create `tools/wallpaper_converter/app.py` with GUI and CLI:

```python
import argparse
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import gifpack

FPS_CHOICES = (10, 15, 20, 25)

def convert_file(input_path, output_path=None, fps=15, width=412, height=412, max_duration_ms=None):
    source = Path(input_path)
    target = Path(output_path) if output_path else source.with_suffix(".gifp")
    target.parent.mkdir(parents=True, exist_ok=True)
    duration = max_duration_ms if max_duration_ms is not None else gifpack.default_max_duration_ms(fps)
    blob = gifpack.convert_gif_to_gifpack(str(source), fps_cap=fps, max_duration_ms=duration, width=width, height=height)
    target.write_bytes(blob)
    return target

class ConverterApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Xiaozhi Wallpaper Converter")
        self.geometry("560x240")
        self.input_var = tk.StringVar()
        self.output_var = tk.StringVar()
        self.fps_var = tk.IntVar(value=15)
        self.size_var = tk.StringVar(value="412")
        self.status_var = tk.StringVar(value="Choose a source file.")
        self._build_ui()

    def _build_ui(self):
        frame = ttk.Frame(self, padding=16)
        frame.pack(fill=tk.BOTH, expand=True)
        ttk.Label(frame, text="Source").grid(row=0, column=0, sticky="w")
        ttk.Entry(frame, textvariable=self.input_var, width=56).grid(row=0, column=1, padx=8)
        ttk.Button(frame, text="Browse", command=self.choose_input).grid(row=0, column=2)
        ttk.Label(frame, text="Output").grid(row=1, column=0, sticky="w", pady=(10, 0))
        ttk.Entry(frame, textvariable=self.output_var, width=56).grid(row=1, column=1, padx=8, pady=(10, 0))
        ttk.Button(frame, text="Save As", command=self.choose_output).grid(row=1, column=2, pady=(10, 0))
        ttk.Label(frame, text="FPS").grid(row=2, column=0, sticky="w", pady=(10, 0))
        ttk.Combobox(frame, textvariable=self.fps_var, values=FPS_CHOICES, state="readonly", width=8).grid(row=2, column=1, sticky="w", padx=8, pady=(10, 0))
        ttk.Label(frame, text="Size").grid(row=2, column=1, sticky="w", padx=(110, 0), pady=(10, 0))
        ttk.Combobox(frame, textvariable=self.size_var, values=("412", "64"), state="readonly", width=8).grid(row=2, column=1, sticky="w", padx=(160, 0), pady=(10, 0))
        ttk.Button(frame, text="Convert", command=self.convert).grid(row=3, column=1, sticky="w", padx=8, pady=(18, 0))
        ttk.Label(frame, textvariable=self.status_var).grid(row=4, column=0, columnspan=3, sticky="w", pady=(18, 0))

    def choose_input(self):
        path = filedialog.askopenfilename(filetypes=[("Media files", "*.gif *.mp4 *.mov *.m4v *.webm *.mkv *.png *.jpg *.jpeg"), ("All files", "*.*")])
        if path:
            self.input_var.set(path)
            self.output_var.set(str(Path(path).with_suffix(".gifp")))

    def choose_output(self):
        path = filedialog.asksaveasfilename(defaultextension=".gifp", filetypes=[("GIFP files", "*.gifp")])
        if path:
            self.output_var.set(path)

    def convert(self):
        try:
            size = int(self.size_var.get())
            output = convert_file(self.input_var.get(), self.output_var.get(), int(self.fps_var.get()), size, size)
            self.status_var.set(f"Saved {output}")
            messagebox.showinfo("Conversion complete", f"Saved:\n{output}")
        except Exception as exc:
            self.status_var.set(str(exc))
            messagebox.showerror("Conversion failed", str(exc))

def main(argv=None):
    parser = argparse.ArgumentParser(description="Convert media to Xiaozhi .gifp files.")
    parser.add_argument("input", nargs="?")
    parser.add_argument("--output", "-o")
    parser.add_argument("--fps", type=int, default=15, choices=FPS_CHOICES)
    parser.add_argument("--size", type=int, default=412, choices=(64, 412))
    parser.add_argument("--max-duration-ms", type=int)
    args = parser.parse_args(argv)
    if not args.input:
        ConverterApp().mainloop()
        return 0
    print(convert_file(args.input, args.output, args.fps, args.size, args.size, args.max_duration_ms))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Create README**

Create `tools/wallpaper_converter/README.md`:

```markdown
# Xiaozhi Wallpaper Converter

Converts media into `.gifp` files.

Use `--size 412` for TF-card wallpapers and `--size 64` for built-in default UI animations.

```powershell
python tools\wallpaper_converter\app.py input.gif --output output.gifp --fps 15 --size 412
python tools\wallpaper_converter\app.py defaultUI\idle.gif --output main\boards\waveshare\esp32-s3-touch-lcd-1.46\default_ui\idle.gifp --fps 10 --size 64
```
```

- [ ] **Step 3: Generate default UI resources**

Create directory:

```powershell
New-Item -ItemType Directory -Force main\boards\waveshare\esp32-s3-touch-lcd-1.46\default_ui
```

Run:

```powershell
python tools\wallpaper_converter\app.py defaultUI\idle.gif --output main\boards\waveshare\esp32-s3-touch-lcd-1.46\default_ui\idle.gifp --fps 10 --size 64
python tools\wallpaper_converter\app.py defaultUI\listening.gif --output main\boards\waveshare\esp32-s3-touch-lcd-1.46\default_ui\listening.gifp --fps 10 --size 64
python tools\wallpaper_converter\app.py defaultUI\thinking.gif --output main\boards\waveshare\esp32-s3-touch-lcd-1.46\default_ui\thinking.gifp --fps 10 --size 64
python tools\wallpaper_converter\app.py defaultUI\speaking.gif --output main\boards\waveshare\esp32-s3-touch-lcd-1.46\default_ui\speaking.gifp --fps 10 --size 64
python tools\wallpaper_converter\app.py defaultUI\drooling_face.gif --output main\boards\waveshare\esp32-s3-touch-lcd-1.46\default_ui\drooling_face.gifp --fps 10 --size 64
python tools\wallpaper_converter\app.py defaultUI\sleeping_face.gif --output main\boards\waveshare\esp32-s3-touch-lcd-1.46\default_ui\sleeping_face.gifp --fps 10 --size 64
```

- [ ] **Step 4: Verify tool**

```powershell
python -m unittest discover -s tools\wallpaper_converter\tests -v
python -m py_compile tools\wallpaper_converter\app.py tools\wallpaper_converter\gifpack.py
```

Expected: tests pass and `py_compile` has no output.

---

### Task 3: Implement Shared GIFP Player

**Files:**
- Create: `main/boards/waveshare/esp32-s3-touch-lcd-1.46/gifp_player.h`
- Create: `main/boards/waveshare/esp32-s3-touch-lcd-1.46/gifp_player.cc`
- Copy: `heatshrink_common.h`, `heatshrink_config.h`, `heatshrink_decoder.h`, `heatshrink_decoder.c`

- [ ] **Step 1: Copy heatshrink decoder**

Copy decoder files from `H:\1.28inch_ESP32-2424S012\gif_player_project\firmware\gif_player\` into the board directory.

- [ ] **Step 2: Create GIFP player API**

Create `gifp_player.h`:

```cpp
#ifndef GIFP_PLAYER_H_
#define GIFP_PLAYER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class GifpPlayer {
public:
    GifpPlayer();
    ~GifpPlayer();

    bool StartFile(const std::string& path, int center_x = -1, int center_y = -1);
    bool StartMemory(const uint8_t* data, size_t size, int center_x = -1, int center_y = -1);
    void Stop();
    bool running() const { return task_ != nullptr; }

private:
    enum class SourceType { None, File, Memory };
    SourceType source_type_ = SourceType::None;
    std::string path_;
    const uint8_t* memory_ = nullptr;
    size_t memory_size_ = 0;
    TaskHandle_t task_ = nullptr;
    bool stop_requested_ = false;
    lv_obj_t* image_ = nullptr;
    lv_image_dsc_t image_dsc_{};
    uint16_t* frame_buffer_ = nullptr;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    int center_x_ = -1;
    int center_y_ = -1;

    static void TaskEntry(void* arg);
    void Run();
    bool DecodeAndPlay();
    void DrawFrame();
};

#endif
```

- [ ] **Step 3: Port decoder**

Create `gifp_player.cc` by porting decoder logic from `H:\1.28inch_ESP32-2424S012\gif_player_project\firmware\gif_player\gif_player.ino`.

Required changes:

```text
read source from FILE or memory
accept header width/height 64 or 412
allocate width * height * 2 RGB565 buffer
validate CRC over frame table + frame data
support encodings 1..6
draw with LVGL image centered on the screen
```

The public behavior:

```text
StartFile(path)    -> wallpapers from TF card
StartMemory(data)  -> embedded default UI resources
Stop()             -> stops task, removes LVGL image, frees buffer
```

- [ ] **Step 4: Verify player references**

Run:

```powershell
Select-String -Path main\boards\waveshare\esp32-s3-touch-lcd-1.46\gifp_player.cc -Pattern 'GIFP|ENCODING_RLE_RGB565|StartFile|StartMemory|lv_image_set_src'
```

Expected: all patterns present.

---

### Task 4: Embed And Select Default UI Animations

**Files:**
- Create: `main/boards/waveshare/esp32-s3-touch-lcd-1.46/default_gifp_view.h`
- Create: `main/boards/waveshare/esp32-s3-touch-lcd-1.46/default_gifp_view.cc`
- Modify: `main/boards/waveshare/esp32-s3-touch-lcd-1.46/esp32-s3-touch-lcd-1.46.cc`

- [ ] **Step 1: Create default view header**

```cpp
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
```

- [ ] **Step 2: Create default view implementation**

Use embedded binary symbols created by CMake:

```cpp
#include "default_gifp_view.h"

#include <cstdlib>

extern const uint8_t idle_gifp_start[] asm("_binary_idle_gifp_start");
extern const uint8_t idle_gifp_end[] asm("_binary_idle_gifp_end");
extern const uint8_t listening_gifp_start[] asm("_binary_listening_gifp_start");
extern const uint8_t listening_gifp_end[] asm("_binary_listening_gifp_end");
extern const uint8_t thinking_gifp_start[] asm("_binary_thinking_gifp_start");
extern const uint8_t thinking_gifp_end[] asm("_binary_thinking_gifp_end");
extern const uint8_t speaking_gifp_start[] asm("_binary_speaking_gifp_start");
extern const uint8_t speaking_gifp_end[] asm("_binary_speaking_gifp_end");
extern const uint8_t drooling_face_gifp_start[] asm("_binary_drooling_face_gifp_start");
extern const uint8_t drooling_face_gifp_end[] asm("_binary_drooling_face_gifp_end");
extern const uint8_t sleeping_face_gifp_start[] asm("_binary_sleeping_face_gifp_start");
extern const uint8_t sleeping_face_gifp_end[] asm("_binary_sleeping_face_gifp_end");

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
    if (idle_timer_) {
        esp_timer_delete(idle_timer_);
    }
}

void DefaultGifpView::Show() {
    visible_ = true;
    idle_ticks_ = 0;
    PlayForState(state_);
    esp_timer_stop(idle_timer_);
    ESP_ERROR_CHECK(esp_timer_start_periodic(idle_timer_, 10 * 1000 * 1000));
}

void DefaultGifpView::Hide() {
    visible_ = false;
    esp_timer_stop(idle_timer_);
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
        player_.StartMemory(listening_gifp_start, listening_gifp_end - listening_gifp_start);
    } else if (state == kDeviceStateSpeaking) {
        player_.StartMemory(speaking_gifp_start, speaking_gifp_end - speaking_gifp_start);
    } else if (state == kDeviceStateConnecting || state == kDeviceStateActivating) {
        player_.StartMemory(thinking_gifp_start, thinking_gifp_end - thinking_gifp_start);
    } else {
        player_.StartMemory(idle_gifp_start, idle_gifp_end - idle_gifp_start);
    }
}

void DefaultGifpView::PlayIdleSurprise() {
    if (!visible_ || state_ != kDeviceStateIdle) {
        return;
    }
    if ((esp_random() & 1) == 0) {
        player_.StartMemory(drooling_face_gifp_start, drooling_face_gifp_end - drooling_face_gifp_start);
    } else {
        player_.StartMemory(sleeping_face_gifp_start, sleeping_face_gifp_end - sleeping_face_gifp_start);
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
        lv_async_call([](void* p) {
            static_cast<DefaultGifpView*>(p)->PlayIdleSurprise();
        }, view);
    }
}
```

- [ ] **Step 3: Wire into board**

In `esp32-s3-touch-lcd-1.46.cc`:

```cpp
#include "default_gifp_view.h"
```

Add member:

```cpp
DefaultGifpView default_view_;
```

`ShowDefaultPage()` should:

```cpp
if (wallpaper_manager_ != nullptr) {
    wallpaper_manager_->ShowDefaultPage();
}
default_view_.Show();
default_view_.SetState(Application::GetInstance().GetDeviceState());
```

When entering wallpaper mode, call `default_view_.Hide()`.

---

### Task 5: Add TF Card Wallpaper Manager And BOOT Behavior

**Files:**
- Modify: `config.h`
- Modify: `esp32-s3-touch-lcd-1.46.cc`
- Create: `sd_wallpaper_manager.h/.cc`

- [ ] **Step 1: Add config constants**

```cpp
#define TF_CARD_SPI_HOST        SPI3_HOST
#define TF_CARD_PIN_NUM_MISO    GPIO_NUM_16
#define TF_CARD_PIN_NUM_MOSI    GPIO_NUM_17
#define TF_CARD_PIN_NUM_CLK     GPIO_NUM_14
#define TF_CARD_CS_EXPANDER_PIN IO_EXPANDER_PIN_NUM_3
#define WALLPAPER_DIR           "/sdcard/wallpapers"
```

- [ ] **Step 2: Implement manager**

`SdWallpaperManager` responsibilities:

```text
mount /sdcard using SDSPI
set TCA9554 EXIO3 output high before mount
set EXIO3 low during mounted session because CS is not native GPIO
scan /sdcard/wallpapers/*.gifp sorted by filename
persist selected path in Settings namespace wallpaper/path
StartFile(path) on GifpPlayer
Stop player on ShowDefaultPage()
```

If IDF requires native GPIO CS and mount fails on hardware, replace the first pass with a board-local SD SPI shim that toggles EXIO3 around transactions.

- [ ] **Step 3: Change BOOT behavior**

Use existing `Button` wrapper:

```cpp
Button boot_button_{BOOT_BUTTON_GPIO, false, 2000, 0, false};

boot_button_.OnClick([this]() {
    if (wallpaper_manager_ != nullptr && wallpaper_manager_->SwitchToNext()) {
        default_view_.Hide();
    }
});

boot_button_.OnDoubleClick([this]() {
    ShowDefaultPage();
});

boot_button_.OnLongPress([this]() {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateStarting) {
        EnterWifiConfigMode();
    } else {
        app.ToggleChatState();
    }
});
```

---

### Task 6: Update CMake Embeds And Dependencies

**Files:**
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Add embedded GIFP files**

Add board default UI files to `EMBED_FILES` only when this board is selected:

```cmake
if(CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_1_46)
    list(APPEND DEFAULT_UI_GIFP
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/waveshare/esp32-s3-touch-lcd-1.46/default_ui/idle.gifp"
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/waveshare/esp32-s3-touch-lcd-1.46/default_ui/listening.gifp"
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/waveshare/esp32-s3-touch-lcd-1.46/default_ui/thinking.gifp"
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/waveshare/esp32-s3-touch-lcd-1.46/default_ui/speaking.gifp"
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/waveshare/esp32-s3-touch-lcd-1.46/default_ui/drooling_face.gifp"
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/waveshare/esp32-s3-touch-lcd-1.46/default_ui/sleeping_face.gifp"
    )
endif()
```

Then include `${DEFAULT_UI_GIFP}` in `idf_component_register(EMBED_FILES ...)`.

- [ ] **Step 2: Add dependencies if build requires**

Ensure `fatfs` is present. Add `sdmmc` and `esp_driver_sdspi` to `PRIV_REQUIRES` only if the compiler reports missing components.

---

### Task 7: State Mapping And Verification

**Files:**
- Modify board file if state sync is needed.

- [ ] **Step 1: Add periodic state sync**

Create an `esp_timer` in `CustomBoard` that every 500 ms calls:

```cpp
if (wallpaper_manager_ == nullptr || !wallpaper_manager_->wallpaper_mode()) {
    default_view_.SetState(Application::GetInstance().GetDeviceState());
}
```

- [ ] **Step 2: Run converter tests**

```powershell
python -m unittest discover -s tools\wallpaper_converter\tests -v
python -m py_compile tools\wallpaper_converter\app.py tools\wallpaper_converter\gifpack.py
```

- [ ] **Step 3: Build firmware if ESP-IDF is available**

```powershell
Get-Command idf.py -ErrorAction SilentlyContinue
idf.py set-target esp32s3
idf.py -DBOARD_NAME=esp32-s3-touch-lcd-1.46 build
```

If `idf.py` is unavailable, record `firmware build not run: idf.py unavailable`.

---

## Self-Review Notes

Spec coverage:

- `.gifp` TF wallpapers: Tasks 3 and 5.
- BOOT single/double/long: Task 5.
- Windows GUI/CLI converter: Tasks 1 and 2.
- Built-in default GIF animations from `defaultUI`: Tasks 2, 4, and 6.
- 64x64 centered default UI: Tasks 3 and 4.
- Idle surprise animations: Task 4.
- Verification: Task 7.

Known risks:

- TCA9554 EXIO3 is not native GPIO CS. First pass holds it low while SD is mounted on a dedicated SPI host; hardware testing may require a custom CS shim.
- LVGL image descriptor field names may vary; adjust `GifpPlayer` to the local `lv_image_dsc_t` definition during build.
- The project is not a git repository, so commit steps are conditional and likely skipped.

