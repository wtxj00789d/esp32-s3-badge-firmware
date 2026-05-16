# Badge Android BLE App Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the Android-first Tauri companion app and matching badge firmware BLE file service for immediate BWP/WAV upload, SD file management, download, share/send, and delete.

**Architecture:** Define a small BLE file protocol first, then implement the firmware service and Android app against that contract. The firmware owns SD safety and badge state through `BadgeStorage` and `BadgeApplication`; the Android app owns media preview/conversion UX, BLE transport orchestration, Android file picking, and Android share/send.

**Tech Stack:** ESP-IDF 5.5.2 C++ with NimBLE BLE peripheral, FATFS/SDMMC, FreeRTOS queues; Tauri v2 Android app with TypeScript/React UI, Rust core crate, Android Kotlin plugin for BLE and share/send.

---

## Current Baseline

The main workspace has an already-implemented recording animation in:

```text
main/badge/badge_application.cc
main/badge/badge_application.h
main/badge/badge_defaults.cc
main/badge/badge_defaults.h
```

Do not revert or reimplement that work. BLE tasks may touch `badge_application.*` for command integration, but they must preserve the recording animation members and `DrawRecordingFrame()` behavior.

## File Structure

Create shared Rust core:

```text
crates/badge-core/Cargo.toml
crates/badge-core/src/lib.rs
crates/badge-core/src/bwp.rs
crates/badge-core/src/protocol.rs
crates/badge-core/src/wav.rs
crates/badge-core/tests/bwp_tests.rs
crates/badge-core/tests/protocol_tests.rs
crates/badge-core/tests/wav_tests.rs
```

Create Android app:

```text
apps/badge-android/package.json
apps/badge-android/index.html
apps/badge-android/tsconfig.json
apps/badge-android/vite.config.ts
apps/badge-android/src/App.tsx
apps/badge-android/src/main.tsx
apps/badge-android/src/styles.css
apps/badge-android/src/lib/ble.ts
apps/badge-android/src/lib/media.ts
apps/badge-android/src/lib/types.ts
apps/badge-android/src/components/MediaMaker.tsx
apps/badge-android/src/components/FileManager.tsx
apps/badge-android/src-tauri/Cargo.toml
apps/badge-android/src-tauri/tauri.conf.json
apps/badge-android/src-tauri/src/lib.rs
apps/badge-android/src-tauri/capabilities/default.json
apps/badge-android/src-tauri/gen/android/app/src/main/AndroidManifest.xml
apps/badge-android/src-tauri/gen/android/app/src/main/java/com/badge/app/BadgeBlePlugin.kt
apps/badge-android/src-tauri/gen/android/app/src/main/java/com/badge/app/BadgeSharePlugin.kt
apps/badge-android/src-tauri/gen/android/app/src/main/res/xml/file_paths.xml
```

Modify firmware:

```text
main/CMakeLists.txt
main/badge/badge_application.h
main/badge/badge_application.cc
main/badge/badge_storage.h
main/badge/badge_storage.cc
sdkconfig.defaults.esp32s3
```

Create firmware BLE files:

```text
main/badge/badge_ble_protocol.h
main/badge/badge_ble_protocol.cc
main/badge/badge_ble_file_service.h
main/badge/badge_ble_file_service.cc
```

## Protocol Contract

The first implementation uses four custom GATT characteristics under one service:

```text
Service UUID:              8d6f6d40-42f5-4c24-89a5-9d02a45c3f01
Command write UUID:        8d6f6d41-42f5-4c24-89a5-9d02a45c3f01
Response notify UUID:      8d6f6d42-42f5-4c24-89a5-9d02a45c3f01
Upload data write UUID:    8d6f6d43-42f5-4c24-89a5-9d02a45c3f01
Download data notify UUID: 8d6f6d44-42f5-4c24-89a5-9d02a45c3f01
```

Command characteristic payloads are UTF-8 ASCII lines:

```text
HELLO
STORAGE
LIST /BADGE
LIST /REC
STAT /BADGE/003.BWP
UPLOAD /BADGE/003.BWP 6912020 89abcdef
COMMIT 1
ABORT 1
DOWNLOAD /REC/REC0001.WAV
READ 2 0 512
END 2
DELETE /BADGE/003.BWP
SET_CURRENT 003
REFRESH
```

Response characteristic payloads are single-line JSON objects:

```json
{"ok":true,"cmd":"HELLO","protocol":1,"name":"Badge","max_bwp_bytes":7340032,"max_chunk_size":512}
{"ok":true,"cmd":"UPLOAD","session":1,"path":"/sdcard/BADGE/003.BWP"}
{"ok":false,"cmd":"DELETE","error":"path_not_allowed"}
```

Upload data characteristic payload is binary:

```text
u32 little-endian session_id
u32 little-endian offset
raw payload bytes
```

Download data notify payload is binary:

```text
u32 little-endian session_id
u32 little-endian offset
raw payload bytes
```

All app-facing paths use `/BADGE` and `/REC`. Firmware converts them to `/sdcard/BADGE` and `/sdcard/REC` after validating the path.

## Task 1: Shared Rust Protocol And BWP/WAV Core

**Files:**
- Create: `crates/badge-core/Cargo.toml`
- Create: `crates/badge-core/src/lib.rs`
- Create: `crates/badge-core/src/protocol.rs`
- Create: `crates/badge-core/src/bwp.rs`
- Create: `crates/badge-core/src/wav.rs`
- Create: `crates/badge-core/tests/protocol_tests.rs`
- Create: `crates/badge-core/tests/bwp_tests.rs`
- Create: `crates/badge-core/tests/wav_tests.rs`

- [ ] **Step 1: Create the Rust crate manifest**

Create `crates/badge-core/Cargo.toml`:

```toml
[package]
name = "badge-core"
version = "0.1.0"
edition = "2021"

[dependencies]
crc32fast = "1"
serde = { version = "1", features = ["derive"] }
serde_json = "1"
thiserror = "1"
```

- [ ] **Step 2: Create module exports**

Create `crates/badge-core/src/lib.rs`:

```rust
pub mod bwp;
pub mod protocol;
pub mod wav;
```

- [ ] **Step 3: Add protocol command builders**

Create `crates/badge-core/src/protocol.rs`:

```rust
use serde::{Deserialize, Serialize};

pub const SERVICE_UUID: &str = "8d6f6d40-42f5-4c24-89a5-9d02a45c3f01";
pub const COMMAND_UUID: &str = "8d6f6d41-42f5-4c24-89a5-9d02a45c3f01";
pub const RESPONSE_UUID: &str = "8d6f6d42-42f5-4c24-89a5-9d02a45c3f01";
pub const UPLOAD_DATA_UUID: &str = "8d6f6d43-42f5-4c24-89a5-9d02a45c3f01";
pub const DOWNLOAD_DATA_UUID: &str = "8d6f6d44-42f5-4c24-89a5-9d02a45c3f01";

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DeviceInfo {
    pub protocol: u32,
    pub name: String,
    pub max_bwp_bytes: u32,
    pub max_chunk_size: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CommandResponse {
    pub ok: bool,
    pub cmd: String,
    #[serde(default)]
    pub error: Option<String>,
    #[serde(default)]
    pub session: Option<u32>,
    #[serde(default)]
    pub path: Option<String>,
    #[serde(default)]
    pub entries: Vec<FileEntry>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct FileEntry {
    pub name: String,
    pub path: String,
    pub size: u64,
    pub kind: FileKind,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum FileKind {
    File,
    Directory,
}

pub fn hello_command() -> String {
    "HELLO".to_string()
}

pub fn storage_command() -> String {
    "STORAGE".to_string()
}

pub fn list_command(path: &str) -> String {
    format!("LIST {}", path)
}

pub fn upload_command(path: &str, size: u64, crc32: u32) -> String {
    format!("UPLOAD {} {} {:08x}", path, size, crc32)
}

pub fn commit_command(session: u32) -> String {
    format!("COMMIT {}", session)
}

pub fn abort_command(session: u32) -> String {
    format!("ABORT {}", session)
}

pub fn download_command(path: &str) -> String {
    format!("DOWNLOAD {}", path)
}

pub fn read_command(session: u32, offset: u64, length: u32) -> String {
    format!("READ {} {} {}", session, offset, length)
}

pub fn end_command(session: u32) -> String {
    format!("END {}", session)
}

pub fn delete_command(path: &str) -> String {
    format!("DELETE {}", path)
}

pub fn set_current_command(basename: &str) -> String {
    format!("SET_CURRENT {}", basename)
}

pub fn upload_chunk(session: u32, offset: u32, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(8 + payload.len());
    out.extend_from_slice(&session.to_le_bytes());
    out.extend_from_slice(&offset.to_le_bytes());
    out.extend_from_slice(payload);
    out
}

pub fn parse_response(json: &str) -> serde_json::Result<CommandResponse> {
    serde_json::from_str(json)
}
```

- [ ] **Step 4: Add protocol tests**

Create `crates/badge-core/tests/protocol_tests.rs`:

```rust
use badge_core::protocol::*;

#[test]
fn builds_upload_command_with_lowercase_hex_crc() {
    assert_eq!(
        upload_command("/BADGE/003.BWP", 6912020, 0x89abcdef),
        "UPLOAD /BADGE/003.BWP 6912020 89abcdef"
    );
}

#[test]
fn encodes_upload_chunk_header_little_endian() {
    let encoded = upload_chunk(1, 512, &[0xaa, 0xbb]);
    assert_eq!(encoded, vec![1, 0, 0, 0, 0, 2, 0, 0, 0xaa, 0xbb]);
}

#[test]
fn parses_file_list_response() {
    let response = parse_response(
        r#"{"ok":true,"cmd":"LIST","entries":[{"name":"003.BWP","path":"/BADGE/003.BWP","size":115220,"kind":"file"}]}"#,
    )
    .unwrap();
    assert!(response.ok);
    assert_eq!(response.entries[0].name, "003.BWP");
    assert_eq!(response.entries[0].kind, FileKind::File);
}
```

- [ ] **Step 5: Add BWP encoder**

Create `crates/badge-core/src/bwp.rs`:

```rust
use thiserror::Error;

pub const WIDTH: u16 = 240;
pub const HEIGHT: u16 = 240;
pub const FRAME_BYTES: usize = WIDTH as usize * HEIGHT as usize * 2;
pub const HEADER_BYTES: usize = 16;
pub const DEFAULT_MAX_BWP_BYTES: usize = 7 * 1024 * 1024;
pub const FPS_CANDIDATES: &[u16] = &[20, 15, 12, 10, 8, 6, 5, 4, 3, 2, 1];

#[derive(Debug, Clone)]
pub struct RgbaFrame {
    pub width: u32,
    pub height: u32,
    pub duration_ms: u32,
    pub rgba: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BwpPlan {
    pub fps: u16,
    pub frame_count: usize,
    pub estimated_size: usize,
    pub duration_ms: u32,
}

#[derive(Debug, Error, PartialEq, Eq)]
pub enum BwpError {
    #[error("source contains no frames")]
    EmptySource,
    #[error("source is too long for max BWP size")]
    SourceTooLong,
    #[error("invalid RGBA frame")]
    InvalidFrame,
}

pub fn choose_full_duration_plan(duration_ms: u32, max_bwp_bytes: usize) -> Result<BwpPlan, BwpError> {
    if duration_ms == 0 {
        return Err(BwpError::EmptySource);
    }
    for fps in FPS_CANDIDATES {
        let frame_count = ((duration_ms as u64 * *fps as u64) + 999) / 1000;
        let frame_count = frame_count.max(1) as usize;
        let estimated_size = HEADER_BYTES + frame_count * FRAME_BYTES;
        if estimated_size <= max_bwp_bytes {
            return Ok(BwpPlan {
                fps: *fps,
                frame_count,
                estimated_size,
                duration_ms,
            });
        }
    }
    Err(BwpError::SourceTooLong)
}

pub fn rgb888_to_rgb565_le(r: u8, g: u8, b: u8) -> [u8; 2] {
    let value = (((r as u16) & 0xf8) << 8) | (((g as u16) & 0xfc) << 3) | ((b as u16) >> 3);
    [(value & 0xff) as u8, (value >> 8) as u8]
}

pub fn build_bwp_from_rgb565_frames(frames: &[Vec<u8>], fps: u16) -> Result<Vec<u8>, BwpError> {
    if frames.is_empty() {
        return Err(BwpError::EmptySource);
    }
    let mut out = Vec::with_capacity(HEADER_BYTES + frames.len() * FRAME_BYTES);
    out.extend_from_slice(b"BWP1");
    out.extend_from_slice(&WIDTH.to_le_bytes());
    out.extend_from_slice(&HEIGHT.to_le_bytes());
    out.extend_from_slice(&fps.to_le_bytes());
    out.extend_from_slice(&(frames.len() as u16).to_le_bytes());
    out.extend_from_slice(&(FRAME_BYTES as u32).to_le_bytes());
    for frame in frames {
        if frame.len() != FRAME_BYTES {
            return Err(BwpError::InvalidFrame);
        }
        out.extend_from_slice(frame);
    }
    Ok(out)
}
```

- [ ] **Step 6: Add BWP tests**

Create `crates/badge-core/tests/bwp_tests.rs`:

```rust
use badge_core::bwp::*;

#[test]
fn chooses_20fps_when_full_duration_fits() {
    let plan = choose_full_duration_plan(3000, DEFAULT_MAX_BWP_BYTES).unwrap();
    assert_eq!(plan.fps, 20);
    assert_eq!(plan.frame_count, 60);
    assert_eq!(plan.estimated_size, HEADER_BYTES + 60 * FRAME_BYTES);
}

#[test]
fn lowers_fps_to_preserve_duration() {
    let max_size = HEADER_BYTES + 45 * FRAME_BYTES;
    let plan = choose_full_duration_plan(3000, max_size).unwrap();
    assert_eq!(plan.fps, 15);
    assert_eq!(plan.frame_count, 45);
}

#[test]
fn refuses_source_that_does_not_fit_at_1fps() {
    let max_size = HEADER_BYTES + 3 * FRAME_BYTES;
    let err = choose_full_duration_plan(5000, max_size).unwrap_err();
    assert_eq!(err, BwpError::SourceTooLong);
}

#[test]
fn builds_bwp_header_and_payload() {
    let frame = vec![0u8; FRAME_BYTES];
    let blob = build_bwp_from_rgb565_frames(&[frame], 20).unwrap();
    assert_eq!(&blob[0..4], b"BWP1");
    assert_eq!(u16::from_le_bytes([blob[4], blob[5]]), 240);
    assert_eq!(u16::from_le_bytes([blob[6], blob[7]]), 240);
    assert_eq!(u16::from_le_bytes([blob[8], blob[9]]), 20);
    assert_eq!(u16::from_le_bytes([blob[10], blob[11]]), 1);
    assert_eq!(blob.len(), HEADER_BYTES + FRAME_BYTES);
}
```

- [ ] **Step 7: Add WAV writer from PCM samples**

Create `crates/badge-core/src/wav.rs`:

```rust
use thiserror::Error;

pub const SAMPLE_RATE: u32 = 16_000;
pub const CHANNELS: u16 = 1;
pub const BITS_PER_SAMPLE: u16 = 16;

#[derive(Debug, Error, PartialEq, Eq)]
pub enum WavError {
    #[error("selected range contains no samples")]
    EmptyRange,
}

pub fn clamp_sample_range(sample_count: usize, start_seconds: f32, end_seconds: f32) -> Result<(usize, usize), WavError> {
    let start = (start_seconds.max(0.0) * SAMPLE_RATE as f32).round() as usize;
    let end = (end_seconds.max(0.0) * SAMPLE_RATE as f32).round() as usize;
    let start = start.min(sample_count);
    let end = end.min(sample_count);
    if end <= start {
        return Err(WavError::EmptyRange);
    }
    Ok((start, end))
}

pub fn write_pcm_wav(samples: &[i16]) -> Vec<u8> {
    let data_bytes = (samples.len() * 2) as u32;
    let mut out = Vec::with_capacity(44 + samples.len() * 2);
    out.extend_from_slice(b"RIFF");
    out.extend_from_slice(&(36 + data_bytes).to_le_bytes());
    out.extend_from_slice(b"WAVEfmt ");
    out.extend_from_slice(&16u32.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&CHANNELS.to_le_bytes());
    out.extend_from_slice(&SAMPLE_RATE.to_le_bytes());
    out.extend_from_slice(&(SAMPLE_RATE * CHANNELS as u32 * 2).to_le_bytes());
    out.extend_from_slice(&(CHANNELS * 2).to_le_bytes());
    out.extend_from_slice(&BITS_PER_SAMPLE.to_le_bytes());
    out.extend_from_slice(b"data");
    out.extend_from_slice(&data_bytes.to_le_bytes());
    for sample in samples {
        out.extend_from_slice(&sample.to_le_bytes());
    }
    out
}
```

- [ ] **Step 8: Add WAV tests**

Create `crates/badge-core/tests/wav_tests.rs`:

```rust
use badge_core::wav::*;

#[test]
fn clamps_sample_range_by_seconds() {
    let (start, end) = clamp_sample_range(48_000, 1.0, 2.0).unwrap();
    assert_eq!(start, 16_000);
    assert_eq!(end, 32_000);
}

#[test]
fn writes_16khz_mono_pcm_wav_header() {
    let blob = write_pcm_wav(&[0, 123, -123]);
    assert_eq!(&blob[0..4], b"RIFF");
    assert_eq!(&blob[8..12], b"WAVE");
    assert_eq!(u16::from_le_bytes([blob[22], blob[23]]), 1);
    assert_eq!(u32::from_le_bytes([blob[24], blob[25], blob[26], blob[27]]), 16_000);
    assert_eq!(u16::from_le_bytes([blob[34], blob[35]]), 16);
    assert_eq!(u32::from_le_bytes([blob[40], blob[41], blob[42], blob[43]]), 6);
}
```

- [ ] **Step 9: Run Rust core tests**

Run:

```powershell
cargo test --manifest-path crates/badge-core/Cargo.toml
```

Expected: all tests pass.

- [ ] **Step 10: Commit shared core**

Run:

```powershell
git add crates/badge-core
git commit -m "feat: add badge media protocol core"
```

## Task 2: Firmware Storage APIs For BLE File Operations

**Files:**
- Modify: `main/badge/badge_storage.h`
- Modify: `main/badge/badge_storage.cc`

- [ ] **Step 1: Extend storage API**

Update `main/badge/badge_storage.h` to add file metadata and BLE-safe operations:

```cpp
struct BadgeFileEntry {
    std::string name;
    std::string app_path;
    size_t size = 0;
    bool directory = false;
};

class BadgeStorage {
public:
    static constexpr size_t kMaxBwpBytes = 7 * 1024 * 1024;

    bool Mount();
    bool mounted() const { return mounted_; }
    const std::vector<BadgeMediaItem>& Media() const { return media_; }
    void RefreshMedia() { ScanMedia(); }
    std::string NextRecordingPath(int number) const;

    bool ResolveAppPath(const std::string& app_path, std::string& sd_path) const;
    bool ListDir(const std::string& app_path, std::vector<BadgeFileEntry>& entries) const;
    bool StatFile(const std::string& app_path, BadgeFileEntry& entry) const;
    bool DeleteFile(const std::string& app_path);
    bool RenameFile(const std::string& from_app_path, const std::string& to_app_path);
    bool IsAllowedFilePath(const std::string& app_path) const;
    bool FileExistsPublic(const std::string& app_path) const;
```

Keep the existing private `FileExists(const std::string& path) const` helper.

- [ ] **Step 2: Add path validation implementation**

In `main/badge/badge_storage.cc`, add these helpers inside the anonymous namespace:

```cpp
bool StartsWith(const std::string& value, const char* prefix)
{
    return value.rfind(prefix, 0) == 0;
}

bool ContainsTraversal(const std::string& path)
{
    return path.find("..") != std::string::npos || path.find('\\') != std::string::npos;
}

std::string NameFromPath(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return path;
    }
    return path.substr(slash + 1);
}
```

Then implement:

```cpp
bool BadgeStorage::ResolveAppPath(const std::string& app_path, std::string& sd_path) const
{
    if (ContainsTraversal(app_path)) {
        return false;
    }
    if (app_path == "/BADGE") {
        sd_path = kBadgeDir;
        return true;
    }
    if (app_path == "/REC") {
        sd_path = kRecordingDir;
        return true;
    }
    if (StartsWith(app_path, "/BADGE/")) {
        sd_path = std::string(kBadgeDir) + app_path.substr(6);
        return true;
    }
    if (StartsWith(app_path, "/REC/")) {
        sd_path = std::string(kRecordingDir) + app_path.substr(4);
        return true;
    }
    return false;
}

bool BadgeStorage::IsAllowedFilePath(const std::string& app_path) const
{
    if (app_path.empty() || app_path.back() == '/') {
        return false;
    }
    std::string sd_path;
    if (!ResolveAppPath(app_path, sd_path)) {
        return false;
    }
    return app_path != "/BADGE" && app_path != "/REC";
}
```

- [ ] **Step 3: Add list/stat/delete/rename implementation**

Add:

```cpp
bool BadgeStorage::ListDir(const std::string& app_path, std::vector<BadgeFileEntry>& entries) const
{
    entries.clear();
    std::string sd_path;
    if (!mounted_ || !ResolveAppPath(app_path, sd_path)) {
        return false;
    }

    DIR* dir = opendir(sd_path.c_str());
    if (dir == nullptr) {
        return false;
    }

    while (dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        const std::string child_app_path = app_path + "/" + entry->d_name;
        const std::string child_sd_path = sd_path + "/" + entry->d_name;
        struct stat st {};
        if (stat(child_sd_path.c_str(), &st) != 0) {
            continue;
        }
        BadgeFileEntry out;
        out.name = entry->d_name;
        out.app_path = child_app_path;
        out.size = static_cast<size_t>(st.st_size);
        out.directory = S_ISDIR(st.st_mode);
        entries.push_back(out);
    }
    closedir(dir);
    std::sort(entries.begin(), entries.end(), [](const BadgeFileEntry& lhs, const BadgeFileEntry& rhs) {
        if (lhs.directory != rhs.directory) {
            return lhs.directory && !rhs.directory;
        }
        return lhs.name < rhs.name;
    });
    return true;
}

bool BadgeStorage::StatFile(const std::string& app_path, BadgeFileEntry& entry) const
{
    std::string sd_path;
    if (!mounted_ || !ResolveAppPath(app_path, sd_path)) {
        return false;
    }
    struct stat st {};
    if (stat(sd_path.c_str(), &st) != 0) {
        return false;
    }
    entry.name = NameFromPath(app_path);
    entry.app_path = app_path;
    entry.size = static_cast<size_t>(st.st_size);
    entry.directory = S_ISDIR(st.st_mode);
    return true;
}

bool BadgeStorage::DeleteFile(const std::string& app_path)
{
    std::string sd_path;
    if (!mounted_ || !IsAllowedFilePath(app_path) || !ResolveAppPath(app_path, sd_path)) {
        return false;
    }
    return std::remove(sd_path.c_str()) == 0;
}

bool BadgeStorage::RenameFile(const std::string& from_app_path, const std::string& to_app_path)
{
    std::string from_sd_path;
    std::string to_sd_path;
    if (!mounted_ || !IsAllowedFilePath(from_app_path) || !IsAllowedFilePath(to_app_path) ||
        !ResolveAppPath(from_app_path, from_sd_path) || !ResolveAppPath(to_app_path, to_sd_path)) {
        return false;
    }
    return std::rename(from_sd_path.c_str(), to_sd_path.c_str()) == 0;
}

bool BadgeStorage::FileExistsPublic(const std::string& app_path) const
{
    std::string sd_path;
    return ResolveAppPath(app_path, sd_path) && FileExists(sd_path);
}
```

- [ ] **Step 4: Build firmware**

Run:

```powershell
& $py C:\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py build
```

Expected: firmware builds.

- [ ] **Step 5: Commit storage API**

Run:

```powershell
git add main/badge/badge_storage.h main/badge/badge_storage.cc
git commit -m "feat: add badge storage file operations"
```

## Task 3: Firmware BLE Protocol Parser

**Files:**
- Create: `main/badge/badge_ble_protocol.h`
- Create: `main/badge/badge_ble_protocol.cc`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create parser header**

Create `main/badge/badge_ble_protocol.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class BadgeBleCommandType {
    Hello,
    Storage,
    List,
    Stat,
    Upload,
    Commit,
    Abort,
    Download,
    Read,
    End,
    Delete,
    SetCurrent,
    Refresh,
    Invalid,
};

struct BadgeBleCommand {
    BadgeBleCommandType type = BadgeBleCommandType::Invalid;
    std::string path;
    std::string basename;
    uint32_t session = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    size_t size = 0;
    uint32_t crc32 = 0;
    std::string error;
};

BadgeBleCommand ParseBadgeBleCommand(const std::string& line);
std::string BadgeBleOk(const char* cmd);
std::string BadgeBleError(const char* cmd, const char* error);
std::string BadgeBleJsonEscape(const std::string& value);
```

- [ ] **Step 2: Create parser implementation**

Create `main/badge/badge_ble_protocol.cc`:

```cpp
#include "badge_ble_protocol.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace {
uint32_t ParseU32(const std::string& text)
{
    return static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
}

uint32_t ParseHexU32(const std::string& text)
{
    return static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 16));
}
}

BadgeBleCommand ParseBadgeBleCommand(const std::string& line)
{
    std::istringstream in(line);
    std::string op;
    in >> op;

    BadgeBleCommand cmd;
    if (op == "HELLO") {
        cmd.type = BadgeBleCommandType::Hello;
    } else if (op == "STORAGE") {
        cmd.type = BadgeBleCommandType::Storage;
    } else if (op == "LIST") {
        cmd.type = BadgeBleCommandType::List;
        in >> cmd.path;
    } else if (op == "STAT") {
        cmd.type = BadgeBleCommandType::Stat;
        in >> cmd.path;
    } else if (op == "UPLOAD") {
        cmd.type = BadgeBleCommandType::Upload;
        std::string size;
        std::string crc;
        in >> cmd.path >> size >> crc;
        cmd.size = static_cast<size_t>(std::strtoull(size.c_str(), nullptr, 10));
        cmd.crc32 = ParseHexU32(crc);
    } else if (op == "COMMIT") {
        cmd.type = BadgeBleCommandType::Commit;
        std::string session;
        in >> session;
        cmd.session = ParseU32(session);
    } else if (op == "ABORT") {
        cmd.type = BadgeBleCommandType::Abort;
        std::string session;
        in >> session;
        cmd.session = ParseU32(session);
    } else if (op == "DOWNLOAD") {
        cmd.type = BadgeBleCommandType::Download;
        in >> cmd.path;
    } else if (op == "READ") {
        cmd.type = BadgeBleCommandType::Read;
        std::string session;
        std::string offset;
        std::string length;
        in >> session >> offset >> length;
        cmd.session = ParseU32(session);
        cmd.offset = ParseU32(offset);
        cmd.length = ParseU32(length);
    } else if (op == "END") {
        cmd.type = BadgeBleCommandType::End;
        std::string session;
        in >> session;
        cmd.session = ParseU32(session);
    } else if (op == "DELETE") {
        cmd.type = BadgeBleCommandType::Delete;
        in >> cmd.path;
    } else if (op == "SET_CURRENT") {
        cmd.type = BadgeBleCommandType::SetCurrent;
        in >> cmd.basename;
    } else if (op == "REFRESH") {
        cmd.type = BadgeBleCommandType::Refresh;
    } else {
        cmd.error = "unknown_command";
    }
    return cmd;
}

std::string BadgeBleJsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

std::string BadgeBleOk(const char* cmd)
{
    return std::string("{\"ok\":true,\"cmd\":\"") + cmd + "\"}";
}

std::string BadgeBleError(const char* cmd, const char* error)
{
    return std::string("{\"ok\":false,\"cmd\":\"") + cmd + "\",\"error\":\"" + error + "\"}";
}
```

- [ ] **Step 3: Add parser source to CMake**

In `main/CMakeLists.txt`, add:

```cmake
"badge/badge_ble_protocol.cc"
```

inside `set(SOURCES ...)`.

- [ ] **Step 4: Build firmware**

Run:

```powershell
& $py C:\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py build
```

Expected: firmware builds.

- [ ] **Step 5: Commit parser**

Run:

```powershell
git add main/CMakeLists.txt main/badge/badge_ble_protocol.h main/badge/badge_ble_protocol.cc
git commit -m "feat: add badge ble command parser"
```

## Task 4: Firmware BLE File Service

**Files:**
- Create: `main/badge/badge_ble_file_service.h`
- Create: `main/badge/badge_ble_file_service.cc`
- Modify: `main/CMakeLists.txt`
- Modify: `sdkconfig.defaults.esp32s3`

- [ ] **Step 1: Enable NimBLE defaults**

Append to `sdkconfig.defaults.esp32s3`:

```text
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517
```

- [ ] **Step 2: Update CMake for BLE**

In `main/CMakeLists.txt`, add:

```cmake
"badge/badge_ble_file_service.cc"
```

inside `set(SOURCES ...)`.

Update `idf_component_register` to include `bt`:

```cmake
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES nvs_flash fatfs sdmmc esp_driver_sdmmc bt
)
```

- [ ] **Step 3: Create service header**

Create `main/badge/badge_ble_file_service.h`:

```cpp
#pragma once

#include <cstdio>
#include <functional>
#include <string>

#include "badge_storage.h"

class BadgeBleFileService {
public:
    struct Callbacks {
        std::function<void()> refresh_media;
        std::function<bool(const std::string& basename)> set_current;
        std::function<void(bool active)> transfer_active;
    };

    BadgeBleFileService(BadgeStorage& storage, Callbacks callbacks);
    bool Start();

private:
    struct UploadSession {
        uint32_t id = 0;
        std::string app_path;
        std::string temp_app_path;
        FILE* file = nullptr;
        size_t expected_size = 0;
        size_t written = 0;
        uint32_t expected_crc32 = 0;
        bool active = false;
    };

    struct DownloadSession {
        uint32_t id = 0;
        std::string app_path;
        FILE* file = nullptr;
        bool active = false;
    };

    BadgeStorage& storage_;
    Callbacks callbacks_;
    UploadSession upload_;
    DownloadSession download_;
    uint32_t next_session_id_ = 1;

    void HandleCommand(const std::string& line);
    void HandleUploadData(const uint8_t* data, size_t length);
    void SendResponse(const std::string& json);
    void SendDownloadChunk(uint32_t session, uint32_t offset, const uint8_t* data, size_t length);
    void CloseUpload(bool remove_temp);
    void CloseDownload();
};
```

- [ ] **Step 4: Create service skeleton**

Create `main/badge/badge_ble_file_service.cc` with NimBLE initialization and handler stubs:

```cpp
#include "badge_ble_file_service.h"

#include <cstring>

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_err.h>
#include <esp_log.h>
#include <host/ble_hs.h>
#include <host/ble_uuid.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#include "badge_ble_protocol.h"

namespace {
constexpr const char* TAG = "BadgeBleFileService";
constexpr uint16_t kMaxChunkSize = 512;
BadgeBleFileService* g_service = nullptr;

void HostTask(void* param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}
}

BadgeBleFileService::BadgeBleFileService(BadgeStorage& storage, Callbacks callbacks)
    : storage_(storage), callbacks_(std::move(callbacks))
{
}

bool BadgeBleFileService::Start()
{
    g_service = this;
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return false;
    }
    ble_svc_gap_device_name_set("Badge-BLE");
    ble_svc_gap_init();
    ble_svc_gatt_init();
    nimble_port_freertos_init(HostTask);
    ESP_LOGI(TAG, "BLE file service started");
    return true;
}

void BadgeBleFileService::HandleCommand(const std::string& line)
{
    const BadgeBleCommand command = ParseBadgeBleCommand(line);
    if (command.type == BadgeBleCommandType::Hello) {
        SendResponse("{\"ok\":true,\"cmd\":\"HELLO\",\"protocol\":1,\"name\":\"Badge\",\"max_bwp_bytes\":7340032,\"max_chunk_size\":512}");
        return;
    }
    SendResponse(BadgeBleError("UNKNOWN", "unsupported_command"));
}

void BadgeBleFileService::HandleUploadData(const uint8_t* data, size_t length)
{
    (void)data;
    (void)length;
}

void BadgeBleFileService::SendResponse(const std::string& json)
{
    ESP_LOGI(TAG, "BLE response: %s", json.c_str());
}

void BadgeBleFileService::SendDownloadChunk(uint32_t session, uint32_t offset, const uint8_t* data, size_t length)
{
    (void)session;
    (void)offset;
    (void)data;
    (void)length;
}

void BadgeBleFileService::CloseUpload(bool remove_temp)
{
    (void)remove_temp;
}

void BadgeBleFileService::CloseDownload()
{
}
```

This step intentionally starts with a compiling skeleton before adding the full GATT table.

- [ ] **Step 5: Build firmware**

Run:

```powershell
& $py C:\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py build
```

Expected: firmware builds or reports exact NimBLE include/config issues to fix before continuing.

- [ ] **Step 6: Commit BLE skeleton**

Run:

```powershell
git add main/CMakeLists.txt sdkconfig.defaults.esp32s3 main/badge/badge_ble_file_service.h main/badge/badge_ble_file_service.cc
git commit -m "feat: start badge ble file service"
```

## Task 5: Firmware BLE Commands And App State Hooks

**Files:**
- Modify: `main/badge/badge_ble_file_service.cc`
- Modify: `main/badge/badge_application.h`
- Modify: `main/badge/badge_application.cc`

- [ ] **Step 1: Replace action queue payload with command struct**

In `main/badge/badge_application.h`, add:

```cpp
struct BadgeCommand {
    BadgeAction action = BadgeAction::PlaySound;
    char basename[4] = {};
};
```

Change:

```cpp
QueueHandle_t action_queue_ = nullptr;
```

to:

```cpp
QueueHandle_t command_queue_ = nullptr;
```

Add public BLE entry points:

```cpp
    bool RequestRefreshMedia();
    bool RequestSetCurrentBasename(const std::string& basename);
    void SetBleTransferActive(bool active);
```

Keep the recording animation private fields unchanged.

- [ ] **Step 2: Update queue creation and button sending**

In `BadgeApplication::Initialize()`, replace:

```cpp
action_queue_ = xQueueCreate(8, sizeof(BadgeAction));
```

with:

```cpp
command_queue_ = xQueueCreate(8, sizeof(BadgeCommand));
```

When sending a button action, replace `xQueueSend(action_queue_, &action, 0)` with:

```cpp
BadgeCommand command;
command.action = action;
if (xQueueSend(command_queue_, &command, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Badge command queue is full");
}
```

- [ ] **Step 3: Add refresh and set-current handlers**

Extend `BadgeAction`:

```cpp
RefreshMedia,
SetCurrentMedia,
```

Add methods in `badge_application.cc`:

```cpp
bool BadgeApplication::RequestRefreshMedia()
{
    if (command_queue_ == nullptr) {
        return false;
    }
    BadgeCommand command;
    command.action = BadgeAction::RefreshMedia;
    return xQueueSend(command_queue_, &command, 0) == pdTRUE;
}

bool BadgeApplication::RequestSetCurrentBasename(const std::string& basename)
{
    if (basename.size() != 3 || command_queue_ == nullptr) {
        return false;
    }
    BadgeCommand command;
    command.action = BadgeAction::SetCurrentMedia;
    std::memcpy(command.basename, basename.data(), 3);
    command.basename[3] = '\0';
    return xQueueSend(command_queue_, &command, 0) == pdTRUE;
}

void BadgeApplication::SetBleTransferActive(bool active)
{
    if (active && state_ == BadgeState::PlayingWallpaper) {
        current_bwp_.Close();
        StartEmbeddedWallpaper();
    }
}
```

In `HandleAction`, handle:

```cpp
case BadgeAction::RefreshMedia:
    storage_.RefreshMedia();
    if (state_ == BadgeState::PlayingEmbeddedWallpaper && !storage_.Media().empty()) {
        current_media_index_ = 0;
        if (LoadCurrentWallpaper()) {
            settings_.SetCurrentBasename(storage_.Media()[current_media_index_].basename);
            state_ = BadgeState::PlayingWallpaper;
            DrawWallpaperFrame();
        }
    }
    break;
case BadgeAction::SetCurrentMedia:
    storage_.RefreshMedia();
    for (size_t i = 0; i < storage_.Media().size(); ++i) {
        if (storage_.Media()[i].basename == pending_basename_from_command) {
            current_media_index_ = i;
            if (LoadCurrentWallpaper()) {
                settings_.SetCurrentBasename(storage_.Media()[i].basename);
                state_ = BadgeState::PlayingWallpaper;
                DrawWallpaperFrame();
            }
            break;
        }
    }
    break;
```

Use the `BadgeCommand` received in `Run()` to pass `command.basename` into `HandleAction`. If keeping `HandleAction(BadgeAction)` is too narrow, change it to `HandleCommand(const BadgeCommand& command)`.

- [ ] **Step 4: Implement BLE command responses**

In `badge_ble_file_service.cc`, implement command handling for:

```text
STORAGE
LIST
STAT
DELETE
REFRESH
SET_CURRENT
UPLOAD
COMMIT
ABORT
DOWNLOAD
READ
END
```

For `LIST`, emit:

```json
{"ok":true,"cmd":"LIST","entries":[{"name":"003.BWP","path":"/BADGE/003.BWP","size":115220,"kind":"file"}]}
```

For `UPLOAD`, reject paths where `storage_.IsAllowedFilePath(path)` is false or `size > BadgeStorage::kMaxBwpBytes` for `.BWP` files. Open temp path:

```text
/BADGE/.003.BWP.tmp
```

For `COMMIT`, verify written byte count and CRC32 before rename.

- [ ] **Step 5: Implement GATT notifications**

Replace `SendResponse()` and `SendDownloadChunk()` logging with notify calls through the response and download characteristics. The implementation must split JSON responses longer than the negotiated notify payload into multiple notifications only if Android side can reassemble them. Keep first-version JSON responses under 512 bytes where possible.

- [ ] **Step 6: Build firmware**

Run:

```powershell
& $py C:\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py build
```

Expected: firmware builds.

- [ ] **Step 7: Commit firmware BLE commands**

Run:

```powershell
git add main/badge/badge_application.h main/badge/badge_application.cc main/badge/badge_ble_file_service.cc
git commit -m "feat: implement badge ble file commands"
```

## Task 6: Tauri Android App Shell

**Files:**
- Create: `apps/badge-android/package.json`
- Create: `apps/badge-android/index.html`
- Create: `apps/badge-android/tsconfig.json`
- Create: `apps/badge-android/vite.config.ts`
- Create: `apps/badge-android/src/main.tsx`
- Create: `apps/badge-android/src/App.tsx`
- Create: `apps/badge-android/src/styles.css`
- Create: `apps/badge-android/src-tauri/Cargo.toml`
- Create: `apps/badge-android/src-tauri/tauri.conf.json`
- Create: `apps/badge-android/src-tauri/src/lib.rs`
- Create: `apps/badge-android/src-tauri/capabilities/default.json`

- [ ] **Step 1: Create package manifest**

Create `apps/badge-android/package.json`:

```json
{
  "name": "badge-android",
  "private": true,
  "version": "0.1.0",
  "type": "module",
  "scripts": {
    "dev": "vite --host 127.0.0.1",
    "build": "tsc && vite build",
    "tauri": "tauri",
    "android:init": "tauri android init",
    "android:dev": "tauri android dev",
    "android:build": "tauri android build"
  },
  "dependencies": {
    "@tauri-apps/api": "^2.0.0",
    "@vitejs/plugin-react": "^4.0.0",
    "vite": "^5.0.0",
    "react": "^18.2.0",
    "react-dom": "^18.2.0",
    "lucide-react": "^0.468.0"
  },
  "devDependencies": {
    "@tauri-apps/cli": "^2.0.0",
    "typescript": "^5.0.0"
  }
}
```

- [ ] **Step 2: Create Vite files**

Create `apps/badge-android/index.html`:

```html
<!doctype html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>Badge Manager</title>
  </head>
  <body>
    <div id="root"></div>
    <script type="module" src="/src/main.tsx"></script>
  </body>
</html>
```

Create `apps/badge-android/tsconfig.json`:

```json
{
  "compilerOptions": {
    "target": "ES2020",
    "useDefineForClassFields": true,
    "lib": ["DOM", "DOM.Iterable", "ES2020"],
    "allowJs": false,
    "skipLibCheck": true,
    "esModuleInterop": true,
    "allowSyntheticDefaultImports": true,
    "strict": true,
    "forceConsistentCasingInFileNames": true,
    "module": "ESNext",
    "moduleResolution": "Node",
    "resolveJsonModule": true,
    "isolatedModules": true,
    "noEmit": true,
    "jsx": "react-jsx"
  },
  "include": ["src"],
  "references": []
}
```

Create `apps/badge-android/vite.config.ts`:

```ts
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  clearScreen: false,
  server: {
    port: 1420,
    strictPort: true,
  },
  envPrefix: ["VITE_", "TAURI_"],
  build: {
    target: "es2020",
    minify: "esbuild",
    sourcemap: false,
  },
});
```

- [ ] **Step 3: Create Tauri backend manifest**

Create `apps/badge-android/src-tauri/Cargo.toml`:

```toml
[package]
name = "badge-android"
version = "0.1.0"
edition = "2021"

[lib]
name = "badge_android_lib"
crate-type = ["staticlib", "cdylib", "rlib"]

[build-dependencies]
tauri-build = { version = "2", features = [] }

[dependencies]
badge-core = { path = "../../../crates/badge-core" }
serde = { version = "1", features = ["derive"] }
serde_json = "1"
tauri = { version = "2", features = [] }
```

Create `apps/badge-android/src-tauri/tauri.conf.json`:

```json
{
  "$schema": "https://schema.tauri.app/config/2",
  "productName": "Badge Manager",
  "version": "0.1.0",
  "identifier": "com.badge.app",
  "build": {
    "beforeDevCommand": "npm run dev",
    "devUrl": "http://127.0.0.1:1420",
    "beforeBuildCommand": "npm run build",
    "frontendDist": "../dist"
  },
  "app": {
    "windows": [
      {
        "title": "Badge Manager",
        "width": 390,
        "height": 844,
        "resizable": true
      }
    ],
    "security": {
      "csp": null
    }
  },
  "bundle": {
    "active": true,
    "targets": "all"
  }
}
```

Create `apps/badge-android/src-tauri/capabilities/default.json`:

```json
{
  "$schema": "../gen/schemas/desktop-schema.json",
  "identifier": "default",
  "description": "Default app capability",
  "windows": ["main"],
  "permissions": ["core:default"]
}
```

Create `apps/badge-android/src-tauri/src/lib.rs`:

```rust
#[tauri::command]
fn choose_bwp_plan(duration_ms: u32, max_bwp_bytes: Option<usize>) -> Result<badge_core::bwp::BwpPlan, String> {
    badge_core::bwp::choose_full_duration_plan(
        duration_ms,
        max_bwp_bytes.unwrap_or(badge_core::bwp::DEFAULT_MAX_BWP_BYTES),
    )
    .map_err(|err| err.to_string())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![choose_bwp_plan])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
```

- [ ] **Step 4: Create React entry and two-tab shell**

Create `apps/badge-android/src/main.tsx`:

```tsx
import React from "react";
import ReactDOM from "react-dom/client";
import App from "./App";
import "./styles.css";

ReactDOM.createRoot(document.getElementById("root") as HTMLElement).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
```

Create `apps/badge-android/src/App.tsx`:

```tsx
import { FolderOpen, ImagePlus } from "lucide-react";
import { useState } from "react";

type Tab = "maker" | "files";

function App() {
  const [tab, setTab] = useState<Tab>("maker");

  return (
    <main className="app-shell">
      <header className="top-bar">
        <div>
          <p className="eyebrow">Badge Manager</p>
          <h1>Media Transfer</h1>
        </div>
        <button className="connect-button" type="button">Connect</button>
      </header>

      <nav className="tab-bar" aria-label="Primary">
        <button className={tab === "maker" ? "active" : ""} onClick={() => setTab("maker")} type="button">
          <ImagePlus size={18} />
          Maker
        </button>
        <button className={tab === "files" ? "active" : ""} onClick={() => setTab("files")} type="button">
          <FolderOpen size={18} />
          Files
        </button>
      </nav>

      <section className="content">
        {tab === "maker" ? (
          <div className="panel">
            <h2>Media Maker</h2>
            <p>Choose wallpaper media, preview badge output, trim optional audio, and upload immediately.</p>
          </div>
        ) : (
          <div className="panel">
            <h2>File Manager</h2>
            <p>Browse /BADGE and /REC, then download, share, send, or delete files.</p>
          </div>
        )}
      </section>
    </main>
  );
}

export default App;
```

Create `apps/badge-android/src/styles.css` with the approved palette:

```css
:root {
  --ice: #d9faff;
  --cyan: #00bbf0;
  --blue: #005792;
  --navy: #00204a;
  --surface: #ffffff;
  --danger: #b42318;
  font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  color: var(--navy);
  background: var(--ice);
}

* {
  box-sizing: border-box;
}

body {
  margin: 0;
  min-width: 320px;
  min-height: 100vh;
}

button {
  font: inherit;
}

.app-shell {
  min-height: 100vh;
  background: var(--ice);
  display: flex;
  flex-direction: column;
}

.top-bar {
  background: var(--navy);
  color: white;
  padding: 20px 18px 16px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.eyebrow {
  margin: 0 0 4px;
  font-size: 12px;
  color: var(--cyan);
}

h1,
h2,
p {
  margin: 0;
}

h1 {
  font-size: 24px;
  font-weight: 700;
}

h2 {
  font-size: 20px;
  margin-bottom: 8px;
}

.connect-button {
  border: 0;
  border-radius: 8px;
  background: var(--cyan);
  color: var(--navy);
  padding: 10px 14px;
  font-weight: 700;
}

.tab-bar {
  display: grid;
  grid-template-columns: 1fr 1fr;
  background: white;
  border-bottom: 1px solid rgba(0, 32, 74, 0.12);
}

.tab-bar button {
  border: 0;
  background: white;
  color: var(--blue);
  padding: 12px;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 8px;
  font-weight: 700;
}

.tab-bar button.active {
  background: var(--blue);
  color: white;
}

.content {
  padding: 16px;
}

.panel {
  background: var(--surface);
  border: 1px solid rgba(0, 32, 74, 0.12);
  border-radius: 8px;
  padding: 16px;
}
```

- [ ] **Step 5: Run web build**

Run:

```powershell
Set-Location apps/badge-android
npm install
npm run build
```

Expected: TypeScript and Vite build pass.

- [ ] **Step 6: Commit app shell**

Run:

```powershell
git add apps/badge-android
git commit -m "feat: scaffold badge android tauri app"
```

## Task 7: Android Media Maker UI And Tauri Commands

**Files:**
- Create: `apps/badge-android/src/lib/types.ts`
- Create: `apps/badge-android/src/lib/media.ts`
- Create: `apps/badge-android/src/components/MediaMaker.tsx`
- Modify: `apps/badge-android/src/App.tsx`
- Modify: `apps/badge-android/src-tauri/src/lib.rs`

- [ ] **Step 1: Define UI types**

Create `apps/badge-android/src/lib/types.ts`:

```ts
export interface BwpPlan {
  fps: number;
  frame_count: number;
  estimated_size: number;
  duration_ms: number;
}

export interface BadgeFileEntry {
  name: string;
  path: string;
  size: number;
  kind: "file" | "directory";
}
```

- [ ] **Step 2: Add media helpers**

Create `apps/badge-android/src/lib/media.ts`:

```ts
export function formatBytes(value: number): string {
  if (value >= 1024 * 1024) {
    return `${(value / (1024 * 1024)).toFixed(1)} MB`;
  }
  if (value >= 1024) {
    return `${(value / 1024).toFixed(1)} KB`;
  }
  return `${value} B`;
}

export function formatDuration(ms: number): string {
  return `${(ms / 1000).toFixed(2)}s`;
}
```

- [ ] **Step 3: Add MediaMaker component**

Create `apps/badge-android/src/components/MediaMaker.tsx`:

```tsx
import { invoke } from "@tauri-apps/api/core";
import { Upload } from "lucide-react";
import { useState } from "react";
import type { BwpPlan } from "../lib/types";
import { formatBytes, formatDuration } from "../lib/media";

export function MediaMaker() {
  const [durationMs, setDurationMs] = useState(3000);
  const [plan, setPlan] = useState<BwpPlan | null>(null);
  const [error, setError] = useState<string | null>(null);

  async function calculatePlan() {
    setError(null);
    try {
      const next = await invoke<BwpPlan>("choose_bwp_plan", { durationMs, maxBwpBytes: null });
      setPlan(next);
    } catch (err) {
      setPlan(null);
      setError(String(err));
    }
  }

  return (
    <div className="maker-layout">
      <section className="panel">
        <h2>Wallpaper</h2>
        <div className="preview-frame">
          <span>240 x 240 preview</span>
        </div>
        <label className="field">
          Duration estimate
          <input
            type="number"
            min={1}
            value={durationMs}
            onChange={(event) => setDurationMs(Number(event.target.value))}
          />
        </label>
        <button className="primary-action" type="button" onClick={calculatePlan}>
          Calculate
        </button>
        {plan && (
          <dl className="stats">
            <div><dt>Duration</dt><dd>{formatDuration(plan.duration_ms)}</dd></div>
            <div><dt>FPS</dt><dd>{plan.fps}</dd></div>
            <div><dt>Frames</dt><dd>{plan.frame_count}</dd></div>
            <div><dt>Size</dt><dd>{formatBytes(plan.estimated_size)}</dd></div>
          </dl>
        )}
        {error && <p className="error-text">{error}</p>}
      </section>

      <section className="panel">
        <h2>Audio</h2>
        <p>Audio picking, range preview, and trimming attach to the same upload basename.</p>
      </section>

      <button className="upload-button" type="button">
        <Upload size={18} />
        Upload Now
      </button>
    </div>
  );
}
```

- [ ] **Step 4: Wire component into App**

Add this import to `App.tsx`:

```tsx
import { MediaMaker } from "./components/MediaMaker";
```

Replace the maker branch with:

```tsx
{tab === "maker" ? (
  <MediaMaker />
) : (
  <div className="panel">
    <h2>File Manager</h2>
    <p>Browse /BADGE and /REC, then download, share, send, or delete files.</p>
  </div>
)}
```

- [ ] **Step 5: Add styles for maker**

Append to `styles.css`:

```css
.maker-layout {
  display: grid;
  gap: 14px;
}

.preview-frame {
  width: min(100%, 240px);
  aspect-ratio: 1;
  margin: 12px auto;
  border-radius: 50%;
  background: linear-gradient(135deg, var(--navy), var(--blue));
  color: white;
  display: grid;
  place-items: center;
  font-weight: 700;
}

.field {
  display: grid;
  gap: 6px;
  font-size: 13px;
  font-weight: 700;
}

.field input {
  min-height: 42px;
  border: 1px solid rgba(0, 32, 74, 0.22);
  border-radius: 8px;
  padding: 8px 10px;
}

.primary-action,
.upload-button {
  border: 0;
  border-radius: 8px;
  background: var(--cyan);
  color: var(--navy);
  padding: 12px 14px;
  font-weight: 800;
}

.upload-button {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 8px;
}

.stats {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 8px;
  margin: 12px 0 0;
}

.stats div {
  border: 1px solid rgba(0, 32, 74, 0.12);
  border-radius: 8px;
  padding: 8px;
}

.stats dt {
  font-size: 11px;
  color: var(--blue);
}

.stats dd {
  margin: 2px 0 0;
  font-weight: 800;
}

.error-text {
  margin-top: 8px;
  color: var(--danger);
  font-weight: 700;
}
```

- [ ] **Step 6: Run app build**

Run:

```powershell
Set-Location apps/badge-android
npm run build
cargo test --manifest-path src-tauri/Cargo.toml
```

Expected: both commands pass.

- [ ] **Step 7: Commit maker foundation**

Run:

```powershell
git add apps/badge-android
git commit -m "feat: add badge media maker foundation"
```

## Task 8: Android BLE And File Manager UI

**Files:**
- Create: `apps/badge-android/src/lib/ble.ts`
- Create: `apps/badge-android/src/components/FileManager.tsx`
- Modify: `apps/badge-android/src/App.tsx`
- Create: `apps/badge-android/src-tauri/gen/android/app/src/main/java/com/badge/app/BadgeBlePlugin.kt`

- [ ] **Step 1: Add BLE TypeScript wrapper**

Create `apps/badge-android/src/lib/ble.ts`:

```ts
import { invoke } from "@tauri-apps/api/core";
import type { BadgeFileEntry } from "./types";

export async function connectBadge(): Promise<void> {
  await invoke("ble_connect_badge");
}

export async function listBadgeDir(path: "/BADGE" | "/REC"): Promise<BadgeFileEntry[]> {
  return invoke<BadgeFileEntry[]>("ble_list_dir", { path });
}

export async function downloadBadgeFile(path: string): Promise<string> {
  return invoke<string>("ble_download_file", { path });
}

export async function deleteBadgeFile(path: string): Promise<void> {
  await invoke("ble_delete_file", { path });
}

export async function shareBadgeFile(path: string): Promise<void> {
  await invoke("share_badge_file", { path });
}
```

- [ ] **Step 2: Add FileManager component**

Create `apps/badge-android/src/components/FileManager.tsx`:

```tsx
import { Download, Send, Trash2 } from "lucide-react";
import { useEffect, useState } from "react";
import { deleteBadgeFile, downloadBadgeFile, listBadgeDir, shareBadgeFile } from "../lib/ble";
import { formatBytes } from "../lib/media";
import type { BadgeFileEntry } from "../lib/types";

export function FileManager() {
  const [root, setRoot] = useState<"/BADGE" | "/REC">("/BADGE");
  const [files, setFiles] = useState<BadgeFileEntry[]>([]);
  const [status, setStatus] = useState("Connect to load files");

  async function refresh(nextRoot = root) {
    setStatus("Loading files");
    try {
      const entries = await listBadgeDir(nextRoot);
      setFiles(entries);
      setStatus(`${entries.length} item(s)`);
    } catch (err) {
      setStatus(String(err));
    }
  }

  useEffect(() => {
    refresh(root);
  }, [root]);

  async function remove(path: string) {
    if (!window.confirm(`Delete ${path}?`)) {
      return;
    }
    await deleteBadgeFile(path);
    await refresh(root);
  }

  return (
    <div className="file-layout">
      <div className="segmented">
        <button className={root === "/BADGE" ? "active" : ""} type="button" onClick={() => setRoot("/BADGE")}>BADGE</button>
        <button className={root === "/REC" ? "active" : ""} type="button" onClick={() => setRoot("/REC")}>REC</button>
      </div>
      <p className="file-status">{status}</p>
      <div className="file-list">
        {files.map((file) => (
          <article className="file-row" key={file.path}>
            <div>
              <strong>{file.name}</strong>
              <span>{formatBytes(file.size)}</span>
            </div>
            <div className="file-actions">
              <button aria-label={`Download ${file.name}`} type="button" onClick={() => downloadBadgeFile(file.path)}><Download size={18} /></button>
              <button aria-label={`Share ${file.name}`} type="button" onClick={() => shareBadgeFile(file.path)}><Send size={18} /></button>
              <button aria-label={`Delete ${file.name}`} type="button" className="danger-icon" onClick={() => remove(file.path)}><Trash2 size={18} /></button>
            </div>
          </article>
        ))}
      </div>
    </div>
  );
}
```

- [ ] **Step 3: Use FileManager in App**

In `App.tsx`, add:

```tsx
import { FileManager } from "./components/FileManager";
```

Replace the files branch with:

```tsx
<FileManager />
```

- [ ] **Step 4: Add file manager styles**

Append to `styles.css`:

```css
.file-layout {
  display: grid;
  gap: 12px;
}

.segmented {
  display: grid;
  grid-template-columns: 1fr 1fr;
  border: 1px solid rgba(0, 32, 74, 0.16);
  border-radius: 8px;
  overflow: hidden;
}

.segmented button {
  border: 0;
  min-height: 42px;
  background: white;
  color: var(--blue);
  font-weight: 800;
}

.segmented button.active {
  background: var(--blue);
  color: white;
}

.file-status {
  font-size: 13px;
  color: var(--blue);
}

.file-list {
  display: grid;
  gap: 8px;
}

.file-row {
  background: white;
  border: 1px solid rgba(0, 32, 74, 0.12);
  border-radius: 8px;
  padding: 12px;
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  gap: 8px;
  align-items: center;
}

.file-row strong,
.file-row span {
  display: block;
}

.file-row span {
  color: var(--blue);
  font-size: 12px;
  margin-top: 2px;
}

.file-actions {
  display: flex;
  gap: 4px;
}

.file-actions button {
  width: 36px;
  height: 36px;
  border: 1px solid rgba(0, 32, 74, 0.14);
  border-radius: 8px;
  background: white;
  color: var(--blue);
  display: grid;
  place-items: center;
}

.file-actions button.danger-icon {
  color: var(--danger);
}
```

- [ ] **Step 5: Add Kotlin BLE plugin skeleton**

Create `apps/badge-android/src-tauri/gen/android/app/src/main/java/com/badge/app/BadgeBlePlugin.kt`:

```kotlin
package com.badge.app

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Context
import app.tauri.annotation.Command
import app.tauri.annotation.TauriPlugin
import app.tauri.plugin.Invoke
import app.tauri.plugin.Plugin

@TauriPlugin(
    permissions = [
        Manifest.permission.BLUETOOTH_SCAN,
        Manifest.permission.BLUETOOTH_CONNECT,
        Manifest.permission.ACCESS_FINE_LOCATION
    ]
)
class BadgeBlePlugin(private val context: Context) : Plugin(context) {
    private val bluetoothManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val adapter: BluetoothAdapter? = bluetoothManager.adapter

    @Command
    fun bleConnectBadge(invoke: Invoke) {
        if (adapter == null || !adapter.isEnabled) {
            invoke.reject("bluetooth_unavailable")
            return
        }
        invoke.resolve()
    }

    @Command
    fun bleListDir(invoke: Invoke) {
        invoke.reject("not_connected")
    }

    @Command
    fun bleDownloadFile(invoke: Invoke) {
        invoke.reject("not_connected")
    }

    @Command
    fun bleDeleteFile(invoke: Invoke) {
        invoke.reject("not_connected")
    }
}
```

- [ ] **Step 6: Run app build**

Run:

```powershell
Set-Location apps/badge-android
npm run build
```

Expected: TypeScript build passes. Android plugin compile is verified after `tauri android init` generates the complete Android project.

- [ ] **Step 7: Commit file manager foundation**

Run:

```powershell
git add apps/badge-android
git commit -m "feat: add badge file manager UI"
```

## Task 9: Android Share/Send Plugin

**Files:**
- Create: `apps/badge-android/src-tauri/gen/android/app/src/main/java/com/badge/app/BadgeSharePlugin.kt`
- Create: `apps/badge-android/src-tauri/gen/android/app/src/main/res/xml/file_paths.xml`
- Modify: `apps/badge-android/src-tauri/gen/android/app/src/main/AndroidManifest.xml`

- [ ] **Step 1: Add FileProvider paths**

Create `apps/badge-android/src-tauri/gen/android/app/src/main/res/xml/file_paths.xml`:

```xml
<?xml version="1.0" encoding="utf-8"?>
<paths xmlns:android="http://schemas.android.com/apk/res/android">
    <cache-path name="badge_cache" path="." />
    <files-path name="badge_files" path="." />
</paths>
```

- [ ] **Step 2: Add FileProvider manifest entry**

In `AndroidManifest.xml`, add inside `<application>`:

```xml
<provider
    android:name="androidx.core.content.FileProvider"
    android:authorities="${applicationId}.fileprovider"
    android:exported="false"
    android:grantUriPermissions="true">
    <meta-data
        android:name="android.support.FILE_PROVIDER_PATHS"
        android:resource="@xml/file_paths" />
</provider>
```

- [ ] **Step 3: Add share plugin**

Create `apps/badge-android/src-tauri/gen/android/app/src/main/java/com/badge/app/BadgeSharePlugin.kt`:

```kotlin
package com.badge.app

import android.content.Context
import android.content.Intent
import androidx.core.content.FileProvider
import app.tauri.annotation.Command
import app.tauri.annotation.TauriPlugin
import app.tauri.plugin.Invoke
import app.tauri.plugin.Plugin
import java.io.File

@TauriPlugin
class BadgeSharePlugin(private val context: Context) : Plugin(context) {
    @Command
    fun shareBadgeFile(invoke: Invoke) {
        val path = invoke.parseArgs(String::class.java)
        val file = File(path)
        if (!file.exists() || !file.isFile) {
            invoke.reject("file_not_found")
            return
        }

        val uri = FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", file)
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = "application/octet-stream"
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        val chooser = Intent.createChooser(intent, "Send badge file")
        chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        context.startActivity(chooser)
        invoke.resolve()
    }
}
```

- [ ] **Step 4: Register plugin with Tauri Android project**

Register `BadgeSharePlugin` and `BadgeBlePlugin` in the generated Tauri Android plugin registration location. If the generated project uses plugin auto-discovery through annotations, verify both plugin annotations are included by running Android build.

- [ ] **Step 5: Build Android app**

Run:

```powershell
Set-Location apps/badge-android
npm run android:build
```

Expected: Android build passes. If Android SDK or Tauri mobile prerequisites are missing, record the exact missing prerequisite and continue with web/Rust test verification.

- [ ] **Step 6: Commit share plugin**

Run:

```powershell
git add apps/badge-android
git commit -m "feat: add android badge file sharing"
```

## Task 10: End-To-End Transfer Integration

**Files:**
- Modify: `apps/badge-android/src/lib/ble.ts`
- Modify: `apps/badge-android/src/components/MediaMaker.tsx`
- Modify: `apps/badge-android/src-tauri/src/lib.rs`
- Modify: `apps/badge-android/src-tauri/gen/android/app/src/main/java/com/badge/app/BadgeBlePlugin.kt`
- Modify: `main/badge/badge_ble_file_service.cc`

- [ ] **Step 1: Implement next basename query in app**

Add a TypeScript helper that lists `/BADGE` and finds the first unoccupied basename:

```ts
export function nextEmptyBasename(files: { name: string }[]): string {
  const occupied = new Set<string>();
  for (const file of files) {
    const match = /^(\d{3})\.(BWP|WAV)$/i.exec(file.name);
    if (match) {
      occupied.add(match[1]);
    }
  }
  for (let index = 1; index <= 999; index += 1) {
    const basename = String(index).padStart(3, "0");
    if (!occupied.has(basename)) {
      return basename;
    }
  }
  throw new Error("No empty badge slot");
}
```

- [ ] **Step 2: Implement upload command flow in Kotlin BLE plugin**

Implement this sequence:

```text
HELLO
UPLOAD /BADGE/003.BWP <size> <crc32>
write upload chunks with session and offset
COMMIT <session>
REFRESH
SET_CURRENT 003
```

Use one active transfer at a time and expose progress events to the web UI through Tauri event emission.

- [ ] **Step 3: Implement download and share flow**

For File Manager share/send:

```text
DOWNLOAD /REC/REC0001.WAV
READ <session> <offset> <length>
repeat until complete
END <session>
write file to app cache
launch ACTION_SEND via BadgeSharePlugin
```

- [ ] **Step 4: Verify firmware temp-file safety**

Test this failure path on hardware:

```text
start upload /BADGE/003.BWP
disconnect before COMMIT
reconnect
LIST /BADGE
```

Expected: `/BADGE/003.BWP` is absent or still the old valid file. Temporary `.003.BWP.tmp` must not appear in app file list.

- [ ] **Step 5: Run full verification**

Run:

```powershell
cargo test --manifest-path crates/badge-core/Cargo.toml
Set-Location apps/badge-android
npm run build
Set-Location E:\work\xiaozhi-esp32-2.2.4
& $py C:\Espressif\frameworks\esp-idf-v5.5.2\tools\idf.py build
```

Expected: Rust tests pass, web build passes, firmware builds.

- [ ] **Step 6: Commit integration**

Run:

```powershell
git add apps/badge-android crates/badge-core main/badge main/CMakeLists.txt sdkconfig.defaults.esp32s3
git commit -m "feat: connect badge app ble transfers"
```

## Implementation Order And Parallel Work

1. Task 1 must happen first because both firmware and app use the protocol contract.
2. Tasks 2-5 can be developed as the firmware track.
3. Tasks 6-9 can be developed as the Android app track.
4. Task 10 integrates both tracks on hardware.

If using parallel workers, split ownership like this:

```text
Worker A: crates/badge-core and protocol docs
Worker B: main/badge BLE firmware files and storage APIs
Worker C: apps/badge-android Tauri UI and Android plugins
```

Workers must not edit each other's owned paths without coordination.

## Verification Checklist

- Rust core tests pass.
- Android web build passes.
- Android native build passes when Tauri mobile prerequisites are installed.
- Firmware `idf.py build` passes.
- Badge advertises BLE service.
- Android app connects to badge.
- Media Maker previews conversion parameters and finds the next empty basename.
- Upload writes temp file, verifies CRC, renames, refreshes media, and can set current wallpaper.
- File Manager lists `/BADGE` and `/REC`.
- File Manager downloads and share/sends a file through Android.
- File Manager deletes a file after confirmation.
- Existing BOOT single/double/long behavior remains intact.
- Existing recording animation remains intact.
