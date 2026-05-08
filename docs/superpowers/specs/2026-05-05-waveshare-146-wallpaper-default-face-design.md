# Waveshare 1.46 Wallpaper And Default Face Design

## Scope

Target board: `Waveshare ESP32-S3-Touch-LCD-1.46`.

This design covers two user-facing changes:

- BOOT button behavior changes for wallpaper and default-page switching.
- A new default page using built-in Cabin-inspired GIF animations.

It also covers a Windows GUI tool that converts user media into the `.gifp` animation format used by the wallpaper feature.

## Goals

- Use the board's onboard TF card slot for user wallpaper animations.
- Support only `.gifp` files for wallpapers, avoiding runtime GIF/PNG decoding risk.
- Keep the default page independent from user wallpapers.
- Replace the current emoji/text default UI with built-in animated GIFP face animations.
- Provide a Windows GUI converter, with command-line support, so users can create `.gifp` files easily.

## Non-Goals

- Do not support Bluetooth audio.
- Do not support raw `.gif` or `.png` as firmware-side wallpapers in the first version.
- Do not copy PRAGMATA/Cabin IP assets directly.
- Do not make the Windows tool flash firmware or write directly to the TF card in the first version.

## Hardware Notes

The Waveshare documentation identifies the onboard TF card slot as SPI-based:

```text
TF Card       ESP32-S3
--------------------------
SD_D0 / MISO  GPIO16
SD_CMD / MOSI GPIO17
SD_SCK / SCLK GPIO14
SD_D3 / CS    EXIO3
SD_D1         NC
SD_D2         NC
```

The current board code initializes the TCA9554 IO expander for LCD/touch reset on `EXIO0 | EXIO1`. The implementation must also configure `EXIO3` as the TF-card chip select line.

## User Interaction

BOOT button behavior:

```text
BOOT single click  -> switch to the next .gifp wallpaper from /sdcard/wallpapers/
BOOT double click  -> call ShowDefaultPage(), returning to the default Cabin-like face page
BOOT long press    -> if starting, enter Wi-Fi config mode; otherwise toggle chat state
```

Wallpaper mode is user-selected and should not be interrupted automatically by normal state changes. The user returns to the default page with BOOT double click.

If no valid `.gifp` files exist, or the selected wallpaper fails validation/loading, the firmware shows an error notification and keeps the current page unchanged.

## Wallpaper Storage

Wallpaper files live on the TF card:

```text
/sdcard/wallpapers/
  001.gifp
  002.gifp
  003.gifp
```

Files are sorted by filename for predictable cycling.

The selected wallpaper is persisted in NVS so the device can restore it after reboot. If the saved file is missing or invalid at boot, the device falls back to the default page.

## GIFP Format

The `.gifp` format is derived from the existing `H:\1.28inch_ESP32-2424S012\gif_player_project` implementation, adapted for this board:

- Magic: `GIFP`
- Version: `1`
- Target size: `412 x 412`
- Pixel format: `RGB565`
- First frame stores a full frame.
- Later frames may use changed rectangles.
- Supported frame encodings:
  - RGB565 RLE
  - delta RGB565 RLE
  - RGB565 heatshrink
  - delta RGB565 heatshrink
  - indexed heatshrink
  - delta indexed heatshrink
- File contains a header, frame table, frame data, and CRC32.

For `412 x 412`, one RGB565 frame buffer is:

```text
412 * 412 * 2 = 339,488 bytes
```

The firmware should allocate one frame buffer and decode frames sequentially. It should reject incompatible dimensions, bad magic, unsupported encodings, invalid table bounds, and CRC mismatches.

## Wallpaper Playback

The wallpaper player runs only in wallpaper mode.

Playback flow:

```text
open .gifp from TF card
  -> validate header and frame table
  -> validate CRC
  -> allocate RGB565 frame buffer
  -> create/show wallpaper surface
  -> decode frame
  -> draw full frame or updated rectangle
  -> wait frame delay
  -> loop
```

When leaving wallpaper mode, stop the playback task/timer, close the file, release frame buffers, and hide/delete the wallpaper surface.

## Default Page

The default page is a Cabin-inspired animated face. Source artwork lives in:

```text
defaultUI/
  idle.gif
  listening.gif
  thinking.gif
  speaking.gif
  drooling_face.gif
  sleeping_face.gif
```

These source GIFs are `64 x 64`. They should be converted into built-in `.gifp` resources for firmware playback. At runtime, the default page shows the animation at its native `64 x 64` size centered on the `412 x 412` screen, with black background around it.

Visual style:

- Black background.
- Small centered Cabin-inspired face animation.
- No text labels.
- No emoji/status text.

Default face mapping:

```text
idle.gif          -> idle / starting
listening.gif     -> listening
thinking.gif      -> connecting / activating / thinking
speaking.gif      -> speaking
drooling_face.gif -> idle long-stay random menu expression
sleeping_face.gif -> idle long-stay random menu expression
```

The default page should be reachable through a stable `ShowDefaultPage()` entry point. The first implementation returns to the built-in GIFP default face view. Later default-page changes should reuse this entry point.

## State Mapping

The face view maps device state to expression animations:

```text
Starting/Idle        -> idle animation
Listening            -> listening animation
Connecting/Activating -> thinking animation
Speaking             -> speaking animation
Long idle            -> occasionally play drooling or sleeping animation, then return to idle
```

If the user is in wallpaper mode, normal state changes do not automatically return to the default page.

## Windows Converter Tool

Add a Windows-first converter:

```text
tools/wallpaper_converter/
  app.py
  gifpack.py
  requirements.txt
  README.md
```

The tool provides a GUI and command-line parameters.

Inputs:

- `.gif`
- `.mp4`
- `.mov`
- `.m4v`
- `.webm`
- `.mkv`
- common static images as single-frame animations if practical

Outputs:

- `.gifp` targeting `412 x 412`
- optional preview artifact if useful

The converter borrows the compression strategy from the 1.28-inch project:

- Crop/scale to target square.
- Sample frames by FPS cap.
- Convert frames to RGB565.
- Encode each frame using the smallest supported encoding.
- Use delta rectangles when beneficial.
- Use palette quantization and light denoising/blur attempts when needed to reduce package size.

The first version only writes output files. Users manually copy `.gifp` files to `/sdcard/wallpapers/`.

## Error Handling

Firmware error cases:

- TF card mount fails: show notification; keep current page.
- `/sdcard/wallpapers/` is missing or empty: show notification; keep current page.
- Selected `.gifp` is invalid: show notification; skip it or keep current page.
- Memory allocation fails: show notification; keep current page.
- Playback decode fails mid-animation: stop playback, show notification, and return to default page or previous stable page.

Windows tool error cases:

- Unsupported input file: show error.
- Missing `ffmpeg` for video input: show setup guidance.
- Conversion result exceeds configured output constraints: suggest lower FPS, shorter duration, or calmer crop.
- Dependency missing: show install command from `requirements.txt`.

## Verification Plan

Firmware:

- Build for `esp32s3` and `Waveshare ESP32-S3-Touch-LCD-1.46`.
- Verify TF card SPI mount path and EXIO3 CS setup.
- Test empty wallpaper directory.
- Test one valid `.gifp`.
- Test multiple `.gifp` files and sorted cycling.
- Test persisted wallpaper restore.
- Test bad magic, bad dimensions, bad CRC, and unsupported encoding.
- Test BOOT single, double, and long press behavior.
- Verify default face animations for all mapped states.

Windows tool:

- Unit test header generation for `412 x 412`.
- Unit test encode/decode round trips for supported frame encodings.
- Unit test CRC generation and validation.
- Test GUI conversion from GIF.
- Test command-line conversion from GIF.
- Test video conversion with `ffmpeg` when available.

## Open Implementation Choices

- Whether the wallpaper renderer draws through a full-screen LVGL canvas/image object or a board-specific LCD draw path.
- Exact LVGL objects used for Cabin-like face arcs and rounded bars.
- Exact user-facing notification text.
