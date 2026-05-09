# Default Assets Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Embed the provided GIF and MP3 as firmware default wallpaper and sound.

**Architecture:** Convert the GIF to 240x240 RGB565 frames at 15fps, then RLE-compress each frame for flash storage and decode one frame at a time into a PSRAM-backed draw buffer. Convert the MP3 to 24kHz mono int16 PCM and embed a bounded first-version clip for default sound playback. SD media remains preferred when present.

**Tech Stack:** ESP-IDF C++, Python asset generation with Pillow and miniaudio or equivalent decoder.

---

### Task 1: Generate Default Asset Sources

**Files:**
- Create: `tools/generate_default_assets.py`
- Create: `main/badge/badge_default_assets.h`
- Create: `main/badge/badge_default_assets.cc`

- [ ] Probe input GIF and MP3.
- [ ] Generate 240x240 RGB565 RLE frames at 15fps.
- [ ] Generate 24kHz mono int16 PCM default sound.

### Task 2: Play Embedded Default Wallpaper

**Files:**
- Modify: `main/CMakeLists.txt`
- Modify: `main/badge/badge_application.h`
- Modify: `main/badge/badge_application.cc`

- [ ] Add a default wallpaper decoder.
- [ ] Use it when SD has no valid BWP.
- [ ] Keep SD BWP behavior unchanged.

### Task 3: Play Embedded Default Sound

**Files:**
- Modify: `main/badge/badge_sound.h`
- Modify: `main/badge/badge_sound.cc`
- Modify: `main/badge/badge_application.cc`

- [ ] Add PCM array playback to `BadgeSoundPlayer`.
- [ ] Use default sound when current wallpaper has no matching WAV.

### Task 4: Build And Flash

- [ ] Run `idf.py build`.
- [ ] Probe COM7 with `chip_id`.
- [ ] Flash only if COM7 is a confirmed ESP32-S3.
