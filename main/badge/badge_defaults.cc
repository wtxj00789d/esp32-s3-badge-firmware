#include "badge_defaults.h"

#include <array>

namespace {
std::array<uint16_t, badge_defaults::kPixelCount> MakePage(uint16_t bg, uint16_t ring, uint16_t center)
{
    std::array<uint16_t, badge_defaults::kPixelCount> page {};
    constexpr int center_x = badge_defaults::kWidth / 2;
    constexpr int center_y = badge_defaults::kHeight / 2;
    constexpr int outer_radius = 92;
    constexpr int inner_radius = 58;
    constexpr int center_radius = 28;
    constexpr int outer_radius_sq = outer_radius * outer_radius;
    constexpr int inner_radius_sq = inner_radius * inner_radius;
    constexpr int center_radius_sq = center_radius * center_radius;

    for (int y = 0; y < badge_defaults::kHeight; ++y) {
        for (int x = 0; x < badge_defaults::kWidth; ++x) {
            const int dx = x - center_x;
            const int dy = y - center_y;
            const int distance_sq = dx * dx + dy * dy;
            uint16_t color = bg;

            if (distance_sq <= center_radius_sq) {
                color = center;
            } else if (distance_sq >= inner_radius_sq && distance_sq <= outer_radius_sq) {
                color = ring;
            }

            page[y * badge_defaults::kWidth + x] = color;
        }
    }

    return page;
}
}

namespace badge_defaults {
const uint16_t* DefaultPage()
{
    static const auto page = MakePage(0x0000, 0x07E0, 0xFFFF);
    return page.data();
}

const uint16_t* RecordingPage()
{
    static const auto page = MakePage(0x0000, 0xF800, 0xFFFF);
    return page.data();
}

const uint16_t* ErrorPage()
{
    static const auto page = MakePage(0x0000, 0xFFE0, 0xF800);
    return page.data();
}
}
