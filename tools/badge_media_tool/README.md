# Badge Media Tool

Windows-side media converter for the ESP32-S3 badge firmware.

It creates files for the SD card layout:

```text
/BADGE/001.BWP
/BADGE/001.WAV
```

The firmware pairs wallpaper and sound by basename, so `001.BWP` uses `001.WAV`.

## GUI

```powershell
python tools\badge_media_tool\app.py
```

## CLI

```powershell
python tools\badge_media_tool\app.py bwp input.gif --output 001.BWP --fps 15 --max-frames 60
python tools\badge_media_tool\app.py wav input.mp3 --output 001.WAV
```

## Formats

- BWP: `BWP1`, 240x240, max 15fps, raw RGB565 frames.
- WAV: 24000Hz, mono, signed 16-bit PCM.

The current firmware loads a BWP file into PSRAM, so the GUI defaults to 60 frames.
