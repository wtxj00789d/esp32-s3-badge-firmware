#pragma once

#include <cstddef>
#include <cstdint>

namespace badge_default_assets {
constexpr int kWidth = 240;
constexpr int kHeight = 240;
constexpr int kFps = 15;
constexpr int kFrameCount = 85;
constexpr int kSoundSampleRate = 16000;

bool DecodeWallpaperFrame(int frame_index, uint16_t* dest, size_t pixel_count);
const int16_t* SoundPcm();
size_t SoundSampleCount();
}
