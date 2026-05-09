#include "badge_defaults.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {
constexpr const char* TAG = "BadgeDefaults";

void FillPage(uint16_t* page, uint16_t bg, uint16_t ring, uint16_t center)
{
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
}

const uint16_t* GetPage(uint16_t bg, uint16_t ring, uint16_t center)
{
    uint16_t* page = static_cast<uint16_t*>(heap_caps_malloc(
        badge_defaults::kPixelCount * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (page == nullptr) {
        page = static_cast<uint16_t*>(heap_caps_malloc(
            badge_defaults::kPixelCount * sizeof(uint16_t),
            MALLOC_CAP_8BIT));
    }
    if (page == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate default page buffer");
        return nullptr;
    }
    FillPage(page, bg, ring, center);
    return page;
}
}

namespace badge_defaults {
const uint16_t* DefaultPage()
{
    static const uint16_t* page = GetPage(0x0000, 0x07E0, 0xFFFF);
    return page;
}

const uint16_t* RecordingPage()
{
    static const uint16_t* page = GetPage(0x0000, 0xF800, 0xFFFF);
    return page;
}

const uint16_t* ErrorPage()
{
    static const uint16_t* page = GetPage(0x0000, 0xFFE0, 0xF800);
    return page;
}
}
