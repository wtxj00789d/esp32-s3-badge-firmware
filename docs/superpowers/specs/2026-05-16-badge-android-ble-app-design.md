# Badge Android BLE App Design

Date: 2026-05-16
Target firmware: Badge firmware for Spotpear ESP32-S3 1.28 inch round LCD box
Target app: Android first, desktop later

## Goal

Build an Android app that manages badge media over BLE. The app has two tabs:

- Media Maker: prepare a wallpaper and optional sound, preview the result, and upload immediately to the badge.
- File Manager: browse the badge SD card, download files, share or send files through Android, and delete files.

The app is a lightweight companion utility. It is not a social sharing app, a heavy project editor, or a long-term local media library.

## Product Decisions

- Use Tauri v2 for the app shell and web UI.
- Use a Rust core library for reusable media conversion, CRC, and BLE protocol data structures.
- Use an Android Kotlin Tauri plugin for native BLE, Android permissions, system file picking, and Android share/send intents.
- Build Android first. Desktop support is a later step that should reuse the Rust core.
- The Media Maker page uploads generated files immediately. It does not keep a persistent work library.
- Temporary conversion output may stay in app cache only for retry after a failed upload.
- File Manager supports share/send for every file it can access.

## Non-Goals For First App

- No iOS implementation.
- No desktop implementation.
- No cloud sync.
- No account system.
- No community or social feed.
- No persistent local project library.
- No manual crop editor.
- No video input in the first Android app.
- No Wi-Fi provisioning, BluFi, OTA, MQTT, WebSocket, or Xiaozhi chat features.

## Architecture

```text
Android App
  |
  +-- Tauri v2 Web UI
  |     +-- Media Maker tab
  |     +-- File Manager tab
  |     +-- connection/status/progress UI
  |
  +-- Rust Core
  |     +-- BWP encoder
  |     +-- image/frame conversion
  |     +-- WAV trim/export
  |     +-- CRC32
  |     +-- BLE command/response models
  |
  +-- Android Kotlin Plugin
        +-- BLE scan/connect/GATT transport
        +-- Android file picker
        +-- Android media playback helpers where needed
        +-- Android ACTION_SEND / ACTION_SEND_MULTIPLE sharing
        +-- FileProvider content URI handling
```

Firmware adds a small BLE file service that talks to existing badge boundaries:

```text
BadgeBleFileService
  |
  +-- BadgeStorage: list, read, write, delete, rename
  +-- BadgeSettings: current wallpaper basename or index
  +-- BadgeApplication: refresh media list, set current wallpaper safely
```

BLE code must not directly control low-level display, audio, or SD internals. It should use BadgeApplication and BadgeStorage entry points.

## Visual Style

Use this palette as app design tokens:

```text
ice   #d9faff
cyan  #00bbf0
blue  #005792
navy  #00204a
```

Recommended usage:

- `#d9faff`: app background, pale status surfaces, empty states.
- `#00bbf0`: primary actions, progress bars, active connection indicators.
- `#005792`: selected tabs, toolbar accents, important labels.
- `#00204a`: primary text, dark toolbars, high-emphasis surfaces.

The app should feel like a compact utility. Use the palette with restraint: light surfaces, clear hierarchy, blue for state and action. Delete remains a destructive action and requires confirmation.

## Media Maker Tab

The Media Maker tab prepares files for `/sdcard/BADGE` and uploads them immediately.

Primary flow:

```text
connect badge
  -> choose wallpaper source
  -> preview badge crop/output
  -> compute final BWP parameters
  -> preview converted result
  -> optionally choose and trim audio
  -> app finds next empty basename
  -> convert to temporary files
  -> upload to /sdcard/BADGE
  -> refresh badge media list
  -> optionally set the new basename as current wallpaper
```

### Wallpaper Source

Supported input:

```text
.gif
.webp
.png
.jpg
.jpeg
.bmp
```

The first Android app does not support video input and does not expose manual crop. The app automatically center-crops the source to the largest square and resizes to `240x240`.

Static images become one-frame BWP files.

### Wallpaper Preview

The page has two previews:

- Source preview: shows the selected GIF/WebP/static image as it will be sampled by the badge view.
- Converted preview: shows the final full-duration output at the selected/downshifted FPS, after center square crop, `240x240` resize, and RGB565 color approximation.

Preview metadata:

- source duration
- estimated source frame count when available
- final FPS
- final frame count
- estimated BWP size
- target basename such as `003`

### BWP Output

Output format:

```text
magic: BWP1
resolution: 240x240
pixel format: raw RGB565
fps: up to 20
payload: full frames
```

Conversion priority:

1. Preserve full duration.
2. Try full-color `20fps`.
3. If the BWP exceeds the firmware limit, lower FPS.
4. Do not silently trim the beginning of a long animation.

Candidate FPS values:

```text
20, 15, 12, 10, 8, 6, 5, 4, 3, 2, 1
```

The app should ask the connected firmware for `max_bwp_bytes`. Until that exists, use `7 MiB` as the conservative default. If `1fps` still exceeds the limit, the app blocks upload and tells the user the source is too long for the current firmware limit.

Size estimate:

```text
header + frame_count * 240 * 240 * 2
```

### Audio Source

The app supports common Android-decodable audio sources through the Android file picker and native/media decoding path.

Output format for app-created badge sound files:

```text
WAV PCM
16000 Hz
16-bit
mono
```

This matches the current `tools/badge_media_tool` output. The firmware sound player should accept this format for APK-created assets. Badge recordings may continue using the firmware recording format and are handled by File Manager as existing files.

Audio flow:

```text
choose audio
  -> load duration
  -> preview original or selected range
  -> choose start/end
  -> preview trimmed result
  -> export temporary WAV
```

The audio file pairs with the wallpaper by basename:

```text
003.BWP -> 003.WAV
```

### Numbering

When connected, the app scans `/sdcard/BADGE` and chooses the first empty three-digit basename.

Occupied basename rule:

```text
003 is occupied if /sdcard/BADGE/003.BWP exists
003 is occupied if /sdcard/BADGE/003.WAV exists
```

If `001` and `002` are occupied, the next upload defaults to `003`.

The first version should avoid manual numbering in the main flow. Advanced override can be added later if it becomes necessary.

### Upload Behavior

Media Maker uploads generated files immediately. It does not expose share/save as primary actions.

Temporary output lifecycle:

- Write conversion output to app cache.
- Upload to badge.
- On success, clear temporary conversion output when it is no longer needed.
- On failure, keep the temporary output for retry within the current session.

If both wallpaper and sound are selected, upload them as one basename group:

```text
/sdcard/BADGE/003.BWP
/sdcard/BADGE/003.WAV
```

If only wallpaper is selected, upload only `003.BWP`.

## File Manager Tab

The File Manager tab is a direct view of badge SD files.

Primary roots:

```text
/BADGE
/REC
```

Every file supports:

- download to phone
- share/send
- delete

For files that still live on the badge:

```text
share/send
  -> download to app cache
  -> expose via FileProvider content URI
  -> launch Android ACTION_SEND
```

For files already downloaded:

```text
share/send
  -> expose cached/local file via FileProvider
  -> launch Android ACTION_SEND
```

Batch sharing can use `ACTION_SEND_MULTIPLE` later. The first version can support single-file sharing only if that keeps the implementation smaller.

The UI may show `/BADGE` both ways:

- grouped media view, such as `003.BWP + 003.WAV`
- raw file view for exact file operations

The raw view is the source of truth.

## BLE File Service

The firmware exposes a custom BLE GATT service for file operations and status. The exact UUIDs can be assigned during implementation, but the contract should keep commands stable.

Recommended command set:

```text
GetDeviceInfo
GetStorageInfo
ListDir(path)
GetFileInfo(path)
BeginUpload(path, size, crc32)
WriteChunk(session_id, offset, data)
CommitUpload(session_id)
AbortUpload(session_id)
BeginDownload(path)
ReadChunk(session_id, offset, length)
EndDownload(session_id)
Delete(path)
SetCurrentBadgeItem(basename)
RefreshMediaList
```

Recommended device info fields:

```text
protocol_version
firmware_name
firmware_version
mtu
max_chunk_size
max_bwp_bytes
supports_share_download
supports_set_current
```

Upload safety:

```text
BeginUpload /sdcard/BADGE/003.BWP
  -> firmware opens /sdcard/BADGE/.003.BWP.tmp

WriteChunk...
  -> firmware writes by offset

CommitUpload
  -> firmware verifies size and crc32
  -> firmware renames .003.BWP.tmp to 003.BWP
  -> firmware refreshes media list if path is under /sdcard/BADGE
```

Partial or failed uploads must not replace valid files.

Delete safety:

- Reject paths outside `/sdcard/BADGE` and `/sdcard/REC`.
- Reject directory traversal.
- Ask for confirmation in the app before delete.
- Stop current playback or switch away if deleting the current BWP.

## BLE Transport

The app should request a larger MTU when possible and adapt to the negotiated value.

Transfer properties:

- single active transfer at a time for first version
- resumable uploads can be added later
- progress is byte-based
- every committed upload is verified with CRC32
- downloads verify byte count and optional CRC when firmware provides one

During upload/download, the firmware should avoid simultaneous SD-heavy playback where it can cause stalls. If needed, BadgeApplication can pause BWP animation while a transfer is active and resume afterward.

## Firmware Integration

Firmware changes should preserve the existing badge architecture:

```text
BadgeApplication owns product state
BadgeStorage owns SD operations
BadgeBleFileService owns BLE protocol and transfer sessions
```

Required additions:

- BLE init and advertising for badge file service.
- File list/read/write/delete APIs in BadgeStorage.
- Atomic temp-file upload path.
- Media refresh API after `/BADGE` changes.
- Set-current API that validates the target basename before changing settings.
- Status API for storage free space and BWP limit.

The BLE service should not reintroduce Wi-Fi, Xiaozhi chat, OTA, LVGL, or network protocols.

## Error Handling

No badge connected:

- Media Maker can preview and compute parameters if local media is selected.
- Upload is disabled until connected.
- Next basename is unavailable until connected.

SD missing:

- File Manager shows the firmware-reported SD error.
- Upload and delete are disabled.
- Download is disabled unless firmware exposes fallback files.

Upload interrupted:

- Firmware closes and removes the temp file when possible.
- App shows retry from current temporary output.
- Existing valid files remain untouched.

Converted wallpaper too large:

- App tries lower FPS values first.
- If still too large at `1fps`, app blocks upload and asks the user to choose shorter media.

Delete current wallpaper:

- App warns before deletion.
- Firmware should switch to a valid fallback or embedded default after deletion.

Share/send failure:

- Keep the downloaded cache file if available.
- Show the Android share failure message in app terms.

## Testing Plan

Rust core tests:

1. Encode one-frame BWP from a static image.
2. Encode GIF/WebP frames with full-duration FPS downshift.
3. Verify RGB565 output size and header fields.
4. Verify oversized animation fails after `1fps` if still above limit.
5. Trim audio to 16000 Hz mono signed 16-bit PCM WAV.
6. Verify CRC32 matches firmware-side calculation.

Android app tests:

1. Import static image and preview badge output.
2. Import GIF/WebP and preview source and converted output.
3. Verify full-duration FPS downshift metadata.
4. Trim and preview audio range.
5. Connect to badge over BLE.
6. Auto-select next empty basename from `/BADGE`.
7. Upload `003.BWP`.
8. Upload `003.BWP + 003.WAV`.
9. Refresh File Manager after upload.
10. Download `/REC/REC0001.WAV`.
11. Share/send a downloaded file through Android.
12. Delete a file after confirmation.

Firmware tests:

1. Advertise badge BLE file service.
2. List `/sdcard/BADGE` and `/sdcard/REC`.
3. Upload to temp file, verify CRC, and rename.
4. Reject bad CRC and preserve old file.
5. Download a file in chunks.
6. Delete an allowed file.
7. Reject paths outside allowed roots.
8. Refresh media list after upload/delete.
9. Set current wallpaper by basename.
10. Recover cleanly from BLE disconnect during upload.

## First Implementation Boundary

The Android BLE app phase is successful when:

- The Android app builds as a Tauri v2 mobile app.
- The app can connect to the badge over BLE.
- Media Maker previews static and animated wallpaper sources.
- Media Maker previews final converted wallpaper parameters and playback.
- Media Maker preserves animation duration and lowers FPS before refusing an oversized source.
- Media Maker trims and previews audio.
- The app auto-selects the next empty basename.
- The app immediately uploads generated BWP/WAV files to `/sdcard/BADGE`.
- The firmware accepts safe temp-file uploads and refreshes media afterward.
- File Manager lists `/BADGE` and `/REC`.
- File Manager downloads, shares/sends, and deletes files.
- Existing badge playback, sound, recording, and BOOT button behavior remain intact.

## Later Work

- Desktop app using the same Rust core.
- iOS app using the same Rust core.
- Manual crop override.
- Video input.
- Batch sharing.
- Transfer resume across app restart.
- Optional advanced numbering override.
