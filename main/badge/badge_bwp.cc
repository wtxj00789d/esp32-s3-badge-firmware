#include "badge_bwp.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {
constexpr const char* TAG = "BadgeBwp";
constexpr uint16_t kExpectedWidth = 240;
constexpr uint16_t kExpectedHeight = 240;
constexpr uint16_t kMaxFps = 20;
constexpr uint32_t kExpectedFrameBytes = kExpectedWidth * kExpectedHeight * 2;

#pragma pack(push, 1)
struct BwpHeader {
    char magic[4];
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint16_t frame_count;
    uint32_t frame_bytes;
};
#pragma pack(pop)

static_assert(sizeof(BwpHeader) == 16, "BWP header must remain packed");

uint8_t* AllocateBwp(size_t size)
{
    void* buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == nullptr) {
        ESP_LOGW(TAG, "PSRAM allocation failed for %u bytes; trying internal heap", static_cast<unsigned>(size));
        buffer = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return static_cast<uint8_t*>(buffer);
}
}

BadgeBwp::~BadgeBwp()
{
    Close();
}

bool BadgeBwp::Load(const char* path)
{
    Close();

    if (path == nullptr || path[0] == '\0') {
        ESP_LOGW(TAG, "BWP path is empty");
        return false;
    }

    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGW(TAG, "Failed to open BWP %s: errno=%d", path, errno);
        return false;
    }

    bool ok = false;
    uint8_t* buffer = nullptr;
    size_t file_size = 0;
    long end = 0;

    if (std::fseek(file, 0, SEEK_END) != 0) {
        ESP_LOGW(TAG, "Failed to seek BWP %s", path);
        goto done;
    }

    end = std::ftell(file);
    if (end < 0) {
        ESP_LOGW(TAG, "Failed to size BWP %s", path);
        goto done;
    }

    if (static_cast<unsigned long>(end) > std::numeric_limits<size_t>::max()) {
        ESP_LOGW(TAG, "BWP %s is too large", path);
        goto done;
    }

    file_size = static_cast<size_t>(end);
    if (file_size < sizeof(BwpHeader)) {
        ESP_LOGW(TAG, "BWP %s is too small: %u bytes", path, static_cast<unsigned>(file_size));
        goto done;
    }

    if (std::fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "Failed to rewind BWP %s", path);
        goto done;
    }

    buffer = AllocateBwp(file_size);
    if (buffer == nullptr) {
        ESP_LOGW(TAG, "Failed to allocate %u bytes for BWP %s", static_cast<unsigned>(file_size), path);
        goto done;
    }

    if (std::fread(buffer, 1, file_size, file) != file_size) {
        ESP_LOGW(TAG, "Failed to read complete BWP %s", path);
        goto done;
    }

    {
        const auto* header = reinterpret_cast<const BwpHeader*>(buffer);
        if (std::memcmp(header->magic, "BWP1", 4) != 0) {
            ESP_LOGW(TAG, "Invalid BWP magic: %s", path);
            goto done;
        }

        if (header->width != kExpectedWidth || header->height != kExpectedHeight) {
            ESP_LOGW(TAG, "Unsupported BWP dimensions in %s: %ux%u", path, header->width, header->height);
            goto done;
        }

        if (header->fps < 1 || header->fps > kMaxFps) {
            ESP_LOGW(TAG, "Unsupported BWP fps in %s: %u", path, header->fps);
            goto done;
        }

        if (header->frame_count < 1) {
            ESP_LOGW(TAG, "BWP has no frames: %s", path);
            goto done;
        }

        if (header->frame_bytes != kExpectedFrameBytes) {
            ESP_LOGW(TAG, "Unexpected BWP frame size in %s: %u", path, static_cast<unsigned>(header->frame_bytes));
            goto done;
        }

        if (header->frame_count > (std::numeric_limits<size_t>::max() - sizeof(BwpHeader)) / header->frame_bytes) {
            ESP_LOGW(TAG, "BWP payload size overflows: %s", path);
            goto done;
        }

        const size_t payload_size = static_cast<size_t>(header->frame_count) * header->frame_bytes;
        if (file_size != sizeof(BwpHeader) + payload_size) {
            ESP_LOGW(TAG, "BWP payload size mismatch in %s: file=%u expected=%u",
                path,
                static_cast<unsigned>(file_size),
                static_cast<unsigned>(sizeof(BwpHeader) + payload_size));
            goto done;
        }

        buffer_ = buffer;
        buffer_size_ = file_size;
        frames_ = reinterpret_cast<const uint16_t*>(buffer_ + sizeof(BwpHeader));
        width_ = header->width;
        height_ = header->height;
        fps_ = header->fps;
        frame_count_ = header->frame_count;
        buffer = nullptr;
        ok = true;
        ESP_LOGI(TAG, "Loaded BWP %s: %dx%d fps=%d frames=%d", path, width_, height_, fps_, frame_count_);
    }

done:
    if (buffer != nullptr) {
        heap_caps_free(buffer);
    }
    std::fclose(file);
    return ok;
}

void BadgeBwp::Close()
{
    if (buffer_ != nullptr) {
        heap_caps_free(buffer_);
    }
    buffer_ = nullptr;
    buffer_size_ = 0;
    frames_ = nullptr;
    width_ = 0;
    height_ = 0;
    fps_ = 0;
    frame_count_ = 0;
}

const uint16_t* BadgeBwp::Frame(int index) const
{
    if (!loaded() || index < 0 || index >= frame_count_) {
        return nullptr;
    }

    const size_t frame_offset = static_cast<size_t>(index) * kExpectedFrameBytes / sizeof(uint16_t);
    return frames_ + frame_offset;
}
