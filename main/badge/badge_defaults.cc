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

const char* GlyphRows(char ch)
{
    switch (ch) {
    case 'L':
        return "10000"
               "10000"
               "10000"
               "10000"
               "10000"
               "10000"
               "11111";
    case 'a':
        return "00000"
               "01110"
               "00001"
               "01111"
               "10001"
               "10011"
               "01101";
    case 'd':
        return "00001"
               "00001"
               "01101"
               "10011"
               "10001"
               "10011"
               "01101";
    case 'g':
        return "00000"
               "01101"
               "10011"
               "10001"
               "01111"
               "00001"
               "01110";
    case 'i':
        return "00100"
               "00000"
               "01100"
               "00100"
               "00100"
               "00100"
               "01110";
    case 'n':
        return "00000"
               "00000"
               "10110"
               "11001"
               "10001"
               "10001"
               "10001";
    case 'o':
        return "00000"
               "00000"
               "01110"
               "10001"
               "10001"
               "10001"
               "01110";
    case '.':
        return "00000"
               "00000"
               "00000"
               "00000"
               "00000"
               "01100"
               "01100";
    default:
        return "00000"
               "00000"
               "00000"
               "00000"
               "00000"
               "00000"
               "00000";
    }
}

void DrawGlyph(uint16_t* page, int x, int y, char ch, int scale, uint16_t color)
{
    constexpr int kGlyphWidth = 5;
    constexpr int kGlyphHeight = 7;
    const char* rows = GlyphRows(ch);
    for (int gy = 0; gy < kGlyphHeight; ++gy) {
        for (int gx = 0; gx < kGlyphWidth; ++gx) {
            if (rows[gy * kGlyphWidth + gx] != '1') {
                continue;
            }
            for (int sy = 0; sy < scale; ++sy) {
                const int py = y + gy * scale + sy;
                if (py < 0 || py >= badge_defaults::kHeight) {
                    continue;
                }
                for (int sx = 0; sx < scale; ++sx) {
                    const int px = x + gx * scale + sx;
                    if (px >= 0 && px < badge_defaults::kWidth) {
                        page[py * badge_defaults::kWidth + px] = color;
                    }
                }
            }
        }
    }
}

void DrawText(uint16_t* page, const char* text, int x, int y, int scale, uint16_t color)
{
    constexpr int kGlyphWidth = 5;
    constexpr int kSpacing = 1;
    int cursor = x;
    for (const char* p = text; *p != '\0'; ++p) {
        DrawGlyph(page, cursor, y, *p, scale, color);
        cursor += (kGlyphWidth + kSpacing) * scale;
    }
}

const uint16_t* GetLoadingPage()
{
    uint16_t* page = static_cast<uint16_t*>(heap_caps_calloc(
        badge_defaults::kPixelCount,
        sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (page == nullptr) {
        page = static_cast<uint16_t*>(heap_caps_calloc(
            badge_defaults::kPixelCount,
            sizeof(uint16_t),
            MALLOC_CAP_8BIT));
    }
    if (page == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate loading page buffer");
        return nullptr;
    }

    constexpr char kText[] = "Loading...";
    constexpr int kScale = 3;
    constexpr int kGlyphWidth = 5;
    constexpr int kGlyphHeight = 7;
    constexpr int kSpacing = 1;
    constexpr int kTextLength = 10;
    constexpr int text_width = (kTextLength * kGlyphWidth + (kTextLength - 1) * kSpacing) * kScale;
    constexpr int text_height = kGlyphHeight * kScale;
    constexpr int x = (badge_defaults::kWidth - text_width) / 2;
    constexpr int y = (badge_defaults::kHeight - text_height) / 2;
    DrawText(page, kText, x, y, kScale, 0xFFFF);
    return page;
}
}

namespace badge_defaults {
const uint16_t* DefaultPage()
{
    static const uint16_t* page = GetPage(0x0000, 0x07E0, 0xFFFF);
    return page;
}

const uint16_t* LoadingPage()
{
    static const uint16_t* page = GetLoadingPage();
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
