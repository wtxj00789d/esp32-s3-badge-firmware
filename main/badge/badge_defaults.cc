#include "badge_defaults.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {
constexpr const char* TAG = "BadgeDefaults";
constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kGlyphSpacing = 1;

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
    case '(':
        return "00110"
               "01000"
               "10000"
               "10000"
               "10000"
               "01000"
               "00110";
    case ')':
        return "01100"
               "00010"
               "00001"
               "00001"
               "00001"
               "00010"
               "01100";
    case 'C':
        return "01111"
               "10000"
               "10000"
               "10000"
               "10000"
               "10000"
               "01111";
    case 'E':
        return "11111"
               "10000"
               "10000"
               "11110"
               "10000"
               "10000"
               "11111";
    case 'L':
        return "10000"
               "10000"
               "10000"
               "10000"
               "10000"
               "10000"
               "11111";
    case 'O':
        return "01110"
               "10001"
               "10001"
               "10001"
               "10001"
               "10001"
               "01110";
    case 'R':
        return "11110"
               "10001"
               "10001"
               "11110"
               "10100"
               "10010"
               "10001";
    case '_':
        return "00000"
               "00000"
               "00000"
               "00000"
               "00000"
               "00000"
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
    int cursor = x;
    for (const char* p = text; *p != '\0'; ++p) {
        DrawGlyph(page, cursor, y, *p, scale, color);
        cursor += (kGlyphWidth + kGlyphSpacing) * scale;
    }
}

int TextWidth(const char* text, int scale)
{
    const int length = static_cast<int>(std::strlen(text));
    if (length <= 0) {
        return 0;
    }
    return (length * kGlyphWidth + (length - 1) * kGlyphSpacing) * scale;
}

uint16_t* AllocateBlackPage(const char* name)
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
        ESP_LOGE(TAG, "Failed to allocate %s page buffer", name);
    }
    return page;
}

const uint16_t* GetLoadingPage()
{
    uint16_t* page = AllocateBlackPage("loading");
    if (page == nullptr) {
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

const uint16_t* GetRecordingPage(int frame_index)
{
    constexpr const char* kFaceFrames[] = {
        "( o _ o )",
        "( O _ o )",
        "( o _ O )",
        "( O _ O )",
    };
    constexpr const char* kRecFrames[] = {
        "   REC   ",
        "  REC.   ",
        "  REC..  ",
        " REC...  ",
    };
    constexpr int kFrameCount = sizeof(kFaceFrames) / sizeof(kFaceFrames[0]);
    static const uint16_t* pages[kFrameCount] = {};

    frame_index %= kFrameCount;
    if (frame_index < 0) {
        frame_index += kFrameCount;
    }

    if (pages[frame_index] != nullptr) {
        return pages[frame_index];
    }

    uint16_t* page = AllocateBlackPage("recording");
    if (page == nullptr) {
        return nullptr;
    }

    constexpr int kScale = 4;
    constexpr int kLineGap = 2;
    constexpr int block_height = (kGlyphHeight * 2 + kLineGap) * kScale;
    const int y0 = (badge_defaults::kHeight - block_height) / 2;
    const int y1 = y0 + (kGlyphHeight + kLineGap) * kScale;
    const int face_x = (badge_defaults::kWidth - TextWidth(kFaceFrames[frame_index], kScale)) / 2;
    const int rec_x = (badge_defaults::kWidth - TextWidth(kRecFrames[frame_index], kScale)) / 2;

    DrawText(page, kFaceFrames[frame_index], face_x, y0, kScale, 0xFFFF);
    DrawText(page, kRecFrames[frame_index], rec_x, y1, kScale, 0xFFFF);
    pages[frame_index] = page;
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

const uint16_t* RecordingPage(int frame_index)
{
    return GetRecordingPage(frame_index);
}

const uint16_t* ErrorPage()
{
    static const uint16_t* page = GetPage(0x0000, 0xFFE0, 0xF800);
    return page;
}
}
