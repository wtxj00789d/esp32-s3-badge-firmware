#pragma once

#include <cstddef>
#include <cstdint>

namespace badge_defaults {
constexpr int kWidth = 240;
constexpr int kHeight = 240;
constexpr int kPixelCount = kWidth * kHeight;

const uint16_t* DefaultPage();
const uint16_t* LoadingPage();
const uint16_t* RecordingPage();
const uint16_t* ErrorPage();
}
