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

Packaged executables:

```powershell
tools\badge_media_tool\dist\BadgeMediaTool.exe
tools\badge_media_tool\dist\BadgeMediaToolCLI.exe bwp input.gif --output 001.BWP --fps 15 --max-frames 60
tools\badge_media_tool\dist\BadgeMediaToolCLI.exe wav input.mp3 --output 001.WAV
```

Rebuild:

```powershell
python -m PyInstaller tools\badge_media_tool\BadgeMediaTool.spec --distpath tools\badge_media_tool\dist --workpath tools\badge_media_tool\pyinstaller_build
python -m PyInstaller tools\badge_media_tool\BadgeMediaToolCLI.spec --distpath tools\badge_media_tool\dist --workpath tools\badge_media_tool\pyinstaller_build_cli
```

## Formats

- BWP: `BWP1`, 240x240, max 20fps, raw RGB565 frames.
- WAV: 16000Hz, mono, signed 16-bit PCM.

The current firmware loads a BWP file into PSRAM, so the GUI defaults to 60 frames.
