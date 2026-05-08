#include "gifp_player.h"
#include "config.h"

#include <algorithm>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_timer.h>
#include <esp_lvgl_port.h>
#include <src/misc/cache/instance/lv_image_cache.h>

extern "C" {
#include "heatshrink_decoder.h"
}

#define TAG "GifpPlayer"

namespace {
constexpr uint8_t kEncodingRleRgb565 = 1;
constexpr uint8_t kEncodingDeltaRleRgb565 = 2;
constexpr uint8_t kEncodingHeatshrinkRgb565 = 3;
constexpr uint8_t kEncodingDeltaHeatshrinkRgb565 = 4;
constexpr uint8_t kEncodingIndexedHeatshrink = 5;
constexpr uint8_t kEncodingDeltaIndexedHeatshrink = 6;
constexpr uint8_t kEncodingIndexedRaw = 7;
constexpr uint8_t kEncodingDeltaIndexedRaw = 8;
constexpr uint16_t kFlagTransparentColorKey = 0x0001;
constexpr uint16_t kTransparentRgb565 = 0x0001;
constexpr uint16_t kHeatshrinkInputBufferSize = 256;
constexpr uint8_t kHeatshrinkWindowBits = 11;
constexpr uint8_t kHeatshrinkLookaheadBits = 4;
constexpr uint16_t kDirectDrawRows = 80;

#pragma pack(push, 1)
struct GifpackHeader {
    char magic[4];
    uint16_t version;
    uint16_t header_size;
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;
    uint16_t flags;
    uint32_t table_off;
    uint32_t data_off;
    uint32_t data_size;
    uint32_t crc32;
};

struct FrameEntry {
    uint32_t offset;
    uint32_t size;
    uint16_t delay_ms;
    uint8_t encoding;
    uint8_t synthetic_count;
};

struct DeltaHeader {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
};
#pragma pack(pop)

struct DeleteContext {
    GifpPlayer* player;
    uint32_t generation;
};

uint32_t Crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            uint32_t mask = -(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

bool SupportedEncoding(uint8_t encoding) {
    return encoding == kEncodingRleRgb565 || encoding == kEncodingDeltaRleRgb565 ||
        encoding == kEncodingHeatshrinkRgb565 || encoding == kEncodingDeltaHeatshrinkRgb565 ||
        encoding == kEncodingIndexedHeatshrink || encoding == kEncodingDeltaIndexedHeatshrink ||
        encoding == kEncodingIndexedRaw || encoding == kEncodingDeltaIndexedRaw;
}

bool FullFrameEncoding(uint8_t encoding) {
    return encoding == kEncodingRleRgb565 || encoding == kEncodingHeatshrinkRgb565 ||
        encoding == kEncodingIndexedHeatshrink || encoding == kEncodingIndexedRaw;
}

uint16_t ReadRgb565Le(const uint8_t* data, uint16_t width, uint16_t x, uint16_t y) {
    uint32_t pos = (static_cast<uint32_t>(y) * width + x) * 2;
    return static_cast<uint16_t>(data[pos]) | (static_cast<uint16_t>(data[pos + 1]) << 8);
}

void WriteRgb565Be(uint8_t* dst, uint32_t pixel_index, uint16_t value) {
    dst[pixel_index * 2] = value >> 8;
    dst[pixel_index * 2 + 1] = value & 0xFF;
}

uint16_t SharpenRgb565(uint16_t center, uint16_t up, uint16_t left, uint16_t right, uint16_t down) {
    auto sharpen_channel = [](int c, int a, int b, int d, int e, int max_value) -> int {
        int avg = (a + b + d + e + 2) / 4;
        int diff = c - avg;
        if (diff > -3 && diff < 3) {
            return c;
        }
        return std::clamp(c + diff / 2, 0, max_value);
    };
    int c_r = (center >> 11) & 0x1F;
    int c_g = (center >> 5) & 0x3F;
    int c_b = center & 0x1F;
    int r = sharpen_channel(c_r, (up >> 11) & 0x1F, (left >> 11) & 0x1F, (right >> 11) & 0x1F, (down >> 11) & 0x1F, 31);
    int g = sharpen_channel(c_g, (up >> 5) & 0x3F, (left >> 5) & 0x3F, (right >> 5) & 0x3F, (down >> 5) & 0x3F, 63);
    int b = sharpen_channel(c_b, up & 0x1F, left & 0x1F, right & 0x1F, down & 0x1F, 31);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

} // namespace

bool GifpPlayer::Source::Open() {
    Close();
    if (type == SourceType::File) {
        file = fopen(path.c_str(), "rb");
        return file != nullptr;
    }
    return type == SourceType::Memory && memory != nullptr && size > 0;
}

void GifpPlayer::Source::Close() {
    if (file != nullptr) {
        fclose(file);
        file = nullptr;
    }
}

bool GifpPlayer::Source::Read(uint32_t offset, void* dst, uint32_t len) {
    if (type == SourceType::Memory) {
        if (offset > size || len > size - offset) {
            return false;
        }
        memcpy(dst, memory + offset, len);
        return true;
    }
    if (file == nullptr) {
        return false;
    }
    if (fseek(file, offset, SEEK_SET) != 0) {
        return false;
    }
    return fread(dst, 1, len, file) == len;
}

GifpPlayer::GifpPlayer() = default;

GifpPlayer::~GifpPlayer() {
    Stop();
}

bool GifpPlayer::StartFile(const std::string& path) {
    if (!StopInternal(false)) {
        return false;
    }
    ++object_generation_;
    source_.type = SourceType::File;
    source_.path = path;
    stop_requested_ = false;
    ESP_LOGI(TAG, "Start GIFP file: %s", path.c_str());
    return xTaskCreate(&GifpPlayer::TaskEntry, "gifp_file", 8192, this, 4, &task_) == pdPASS;
}

bool GifpPlayer::StartMemory(const uint8_t* data, size_t size) {
    if (!StopInternal(false)) {
        return false;
    }
    ++object_generation_;
    source_.type = SourceType::Memory;
    source_.memory = data;
    source_.size = size;
    stop_requested_ = false;
    ESP_LOGI(TAG, "Start GIFP memory: %u bytes", static_cast<unsigned>(size));
    return xTaskCreate(&GifpPlayer::TaskEntry, "gifp_mem", 8192, this, 4, &task_) == pdPASS;
}

void GifpPlayer::Stop() {
    StopInternal(true);
}

bool GifpPlayer::StopInternal(bool delete_lvgl_objects) {
    stop_requested_ = true;
    for (int i = 0; task_ != nullptr && i < 300; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (task_ != nullptr) {
        ESP_LOGW(TAG, "Timed out waiting for GIFP playback task to stop");
        return false;
    }
    source_.Close();
    if (frame_buffer_ != nullptr) {
        heap_caps_free(frame_buffer_);
        frame_buffer_ = nullptr;
    }
    if (delete_lvgl_objects) {
        ++object_generation_;
        bool deleted_now = false;
        if (lvgl_port_lock(1000)) {
            DeleteLvglObjects();
            lvgl_port_unlock();
            deleted_now = true;
        } else {
            auto* context = static_cast<DeleteContext*>(heap_caps_malloc(sizeof(DeleteContext), MALLOC_CAP_8BIT));
            if (context != nullptr) {
                context->player = this;
                context->generation = object_generation_;
                lv_async_call(&GifpPlayer::DeleteLvglObjectsAsync, context);
            }
        }
        if (deleted_now && display_buffer_ != nullptr) {
            heap_caps_free(display_buffer_);
            display_buffer_ = nullptr;
            display_buffer_bytes_ = 0;
        }
    }
    return true;
}

void GifpPlayer::DeleteLvglObjectsAsync(void* arg) {
    auto* context = static_cast<DeleteContext*>(arg);
    if (context->player->object_generation_ == context->generation) {
        context->player->DeleteLvglObjects();
    }
    heap_caps_free(context);
}

void GifpPlayer::TaskEntry(void* arg) {
    auto* self = static_cast<GifpPlayer*>(arg);
    self->Run();
    self->task_ = nullptr;
    vTaskDelete(nullptr);
}

void GifpPlayer::Run() {
    if (!source_.Open()) {
        ESP_LOGW(TAG, "Failed to open GIFP source");
        return;
    }
    if (!DecodeAndPlay()) {
        ESP_LOGW(TAG, "GIFP playback stopped because decode failed");
    }
    source_.Close();
}

bool GifpPlayer::DecodeAndPlay() {
    GifpackHeader header{};
    if (!source_.Read(0, &header, sizeof(header))) {
        ESP_LOGW(TAG, "Failed to read GIFP header");
        return false;
    }
    if (memcmp(header.magic, "GIFP", 4) != 0 || header.version != 1 || header.header_size != sizeof(GifpackHeader)) {
        ESP_LOGW(TAG, "Invalid GIFP header magic/version/header_size");
        return false;
    }
    if ((header.flags & ~kFlagTransparentColorKey) != 0) {
        ESP_LOGW(TAG, "Unsupported GIFP flags: 0x%04x", header.flags);
        return false;
    }
    bool default_ui_size = header.width == 64 && header.height == 64;
    bool wallpaper_size = header.width == header.height && header.width > 0 && header.width <= DISPLAY_WIDTH;
    if (!(default_ui_size || wallpaper_size)) {
        ESP_LOGW(TAG, "Unsupported GIFP size %ux%u", header.width, header.height);
        return false;
    }
    if (header.frame_count == 0) {
        ESP_LOGW(TAG, "GIFP has no frames");
        return false;
    }
    ESP_LOGI(TAG, "GIFP header: %ux%u, frames=%u, flags=0x%04x, table=%lu, data_off=%lu, data=%lu, crc=%08lx",
        header.width, header.height, header.frame_count, header.flags,
        static_cast<unsigned long>(header.table_off), static_cast<unsigned long>(header.data_off),
        static_cast<unsigned long>(header.data_size), static_cast<unsigned long>(header.crc32));

    const uint32_t table_size = static_cast<uint32_t>(header.frame_count) * sizeof(FrameEntry);
    const uint32_t table_end = header.table_off + table_size;
    const uint32_t data_end = header.data_off + header.data_size;
    if (header.table_off < header.header_size || table_end < header.table_off ||
        header.data_off < table_end || data_end < header.data_off) {
        ESP_LOGW(TAG, "Invalid GIFP offsets table_end=%lu data_end=%lu", static_cast<unsigned long>(table_end), static_cast<unsigned long>(data_end));
        return false;
    }

    auto* frames = static_cast<FrameEntry*>(heap_caps_malloc(table_size, MALLOC_CAP_8BIT));
    if (frames == nullptr) {
        ESP_LOGW(TAG, "Failed to allocate GIFP frame table: %lu bytes", static_cast<unsigned long>(table_size));
        return false;
    }
    if (!source_.Read(header.table_off, frames, table_size)) {
        ESP_LOGW(TAG, "Failed to read GIFP frame table");
        heap_caps_free(frames);
        return false;
    }

    if (source_.type == SourceType::Memory) {
        constexpr uint32_t kCrcBufferSize = 4096;
        auto* crc_buf = static_cast<uint8_t*>(heap_caps_malloc(kCrcBufferSize, MALLOC_CAP_8BIT));
        if (crc_buf == nullptr) {
            ESP_LOGW(TAG, "Failed to allocate GIFP CRC buffer");
            heap_caps_free(frames);
            return false;
        }
        uint32_t crc = 0xFFFFFFFFU;
        auto crc_range = [&](uint32_t offset, uint32_t len) -> bool {
            uint32_t pos = 0;
            while (pos < len && !stop_requested_) {
                uint32_t chunk = std::min<uint32_t>(kCrcBufferSize, len - pos);
                if (!source_.Read(offset + pos, crc_buf, chunk)) {
                    return false;
                }
                crc = Crc32Update(crc, crc_buf, chunk);
                pos += chunk;
            }
            return !stop_requested_;
        };
        bool crc_ok = crc_range(header.table_off, table_size) && crc_range(header.data_off, header.data_size);
        heap_caps_free(crc_buf);
        if (!crc_ok) {
            if (stop_requested_) {
                heap_caps_free(frames);
                return true;
            }
            ESP_LOGW(TAG, "Failed to read GIFP data for CRC");
            heap_caps_free(frames);
            return false;
        }
        crc ^= 0xFFFFFFFFU;
        if (crc != header.crc32) {
            ESP_LOGW(TAG, "CRC mismatch expected=%08lx actual=%08lx", static_cast<unsigned long>(header.crc32), static_cast<unsigned long>(crc));
            heap_caps_free(frames);
            return false;
        }
    } else {
        ESP_LOGI(TAG, "Skip GIFP CRC for file source");
    }

    for (uint16_t i = 0; i < header.frame_count; ++i) {
        const auto& entry = frames[i];
        if (!SupportedEncoding(entry.encoding) || entry.size == 0 || entry.offset > header.data_size || entry.size > header.data_size - entry.offset) {
            ESP_LOGW(TAG, "Invalid GIFP frame %u entry: offset=%lu size=%lu delay=%u encoding=%u",
                i, static_cast<unsigned long>(entry.offset), static_cast<unsigned long>(entry.size), entry.delay_ms, entry.encoding);
            heap_caps_free(frames);
            return false;
        }
        if (i == 0 && !FullFrameEncoding(entry.encoding)) {
            ESP_LOGW(TAG, "First GIFP frame is not a full frame: encoding=%u", entry.encoding);
            heap_caps_free(frames);
            return false;
        }
    }

    width_ = header.width;
    height_ = header.height;
    transparent_color_key_ = (header.flags & kFlagTransparentColorKey) != 0;
    frame_bytes_ = static_cast<uint32_t>(width_) * height_ * 2;
    const uint32_t pixel_count = static_cast<uint32_t>(width_) * height_;
    const bool direct_panel_output = panel_ != nullptr && !transparent_color_key_ &&
        ((width_ == DISPLAY_WIDTH && height_ == DISPLAY_HEIGHT) ||
            (width_ * 2 == DISPLAY_WIDTH && height_ * 2 == DISPLAY_HEIGHT));
    const uint32_t display_bytes = direct_panel_output ? static_cast<uint32_t>(DISPLAY_WIDTH) * kDirectDrawRows * 2 :
        frame_bytes_ + (transparent_color_key_ ? pixel_count : 0);
    frame_buffer_ = static_cast<uint8_t*>(heap_caps_malloc(frame_bytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (frame_buffer_ == nullptr) {
        frame_buffer_ = static_cast<uint8_t*>(heap_caps_malloc(frame_bytes_, MALLOC_CAP_8BIT));
    }
    if (frame_buffer_ == nullptr) {
        ESP_LOGW(TAG, "Failed to allocate GIFP frame buffer: %lu bytes", static_cast<unsigned long>(frame_bytes_));
        heap_caps_free(frames);
        return false;
    }
    if (display_buffer_ == nullptr || display_buffer_bytes_ != display_bytes) {
        if (display_buffer_ != nullptr) {
            heap_caps_free(display_buffer_);
            display_buffer_ = nullptr;
            display_buffer_bytes_ = 0;
        }
        display_buffer_ = static_cast<uint8_t*>(heap_caps_malloc(display_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (display_buffer_ == nullptr) {
            display_buffer_ = static_cast<uint8_t*>(heap_caps_malloc(display_bytes, MALLOC_CAP_8BIT));
        }
        if (display_buffer_ == nullptr) {
            ESP_LOGW(TAG, "Failed to allocate GIFP display buffer: %lu bytes", static_cast<unsigned long>(display_bytes));
            heap_caps_free(frames);
            return false;
        }
        display_buffer_bytes_ = display_bytes;
    }

    auto read_frame = [&](const FrameEntry& entry, uint32_t rel_offset, void* dst, uint32_t len) -> bool {
        if (rel_offset > entry.size || len > entry.size - rel_offset) {
            return false;
        }
        return source_.Read(header.data_off + entry.offset + rel_offset, dst, len);
    };

    auto decode_heatshrink_buffer = [&](const uint8_t* encoded, uint32_t encoded_size, uint8_t* dst, uint32_t expected_len) -> bool {
        heatshrink_decoder* decoder = heatshrink_decoder_alloc(kHeatshrinkInputBufferSize, kHeatshrinkWindowBits, kHeatshrinkLookaheadBits);
        if (decoder == nullptr) {
            return false;
        }
        bool ok = true;
        uint32_t in_offset = 0;
        uint32_t out_pos = 0;
        while (ok && in_offset < encoded_size) {
            uint32_t chunk = std::min<uint32_t>(kHeatshrinkInputBufferSize, encoded_size - in_offset);
            const uint8_t* input = encoded + in_offset;
            in_offset += chunk;
            uint32_t consumed = 0;
            while (ok && consumed < chunk) {
                size_t sunk = 0;
                if (heatshrink_decoder_sink(decoder, const_cast<uint8_t*>(input + consumed), chunk - consumed, &sunk) < 0) {
                    ok = false;
                    break;
                }
                consumed += sunk;
                while (true) {
                    size_t output_size = 0;
                    uint8_t overflow = 0;
                    uint32_t remaining = expected_len - out_pos;
                    uint8_t* out = remaining > 0 ? dst + out_pos : &overflow;
                    HSD_poll_res poll_res = heatshrink_decoder_poll(decoder, out, remaining > 0 ? remaining : 1, &output_size);
                    if (remaining == 0 && output_size > 0) {
                        ok = false;
                        break;
                    }
                    out_pos += output_size;
                    if (poll_res == HSDR_POLL_EMPTY) {
                        break;
                    }
                    if (poll_res != HSDR_POLL_MORE) {
                        ok = false;
                        break;
                    }
                }
                if (sunk == 0) {
                    break;
                }
            }
        }
        while (ok) {
            HSD_finish_res finish_res = heatshrink_decoder_finish(decoder);
            if (finish_res == HSDR_FINISH_DONE) {
                break;
            }
            if (finish_res != HSDR_FINISH_MORE) {
                ok = false;
                break;
            }
            size_t output_size = 0;
            uint32_t remaining = expected_len - out_pos;
            uint8_t overflow = 0;
            uint8_t* out = remaining > 0 ? dst + out_pos : &overflow;
            HSD_poll_res poll_res = heatshrink_decoder_poll(decoder, out, remaining > 0 ? remaining : 1, &output_size);
            if (poll_res < 0) {
                ok = false;
                break;
            }
            if (remaining == 0 && output_size > 0) {
                ok = false;
                break;
            }
            out_pos += output_size;
        }
        heatshrink_decoder_free(decoder);
        return ok && out_pos == expected_len;
    };

    auto decode_heatshrink = [&](const FrameEntry& entry, uint32_t rel_offset, uint8_t* dst, uint32_t expected_len) -> bool {
        if (rel_offset > entry.size) {
            return false;
        }
        uint32_t encoded_size = entry.size - rel_offset;
        auto* encoded = static_cast<uint8_t*>(heap_caps_malloc(encoded_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (encoded == nullptr) {
            encoded = static_cast<uint8_t*>(heap_caps_malloc(encoded_size, MALLOC_CAP_8BIT));
        }
        if (encoded == nullptr) {
            return false;
        }
        bool ok = read_frame(entry, rel_offset, encoded, encoded_size) &&
            decode_heatshrink_buffer(encoded, encoded_size, dst, expected_len);
        heap_caps_free(encoded);
        return ok;
    };

    auto decode_rle_buffer = [&](const uint8_t* encoded, uint32_t encoded_size, uint8_t* dst, uint32_t expected_pixels) -> bool {
        uint32_t in_pos = 0;
        uint32_t end = encoded_size;
        uint32_t out_pos = 0;
        while (in_pos < end) {
            uint8_t command = encoded[in_pos];
            in_pos += 1;
            uint32_t run = (command & 0x7F) + 1;
            if (command & 0x80) {
                if (in_pos + 2 > end || out_pos + run * 2 > expected_pixels * 2) {
                    return false;
                }
                uint8_t pixel0 = encoded[in_pos];
                uint8_t pixel1 = encoded[in_pos + 1];
                in_pos += 2;
                for (uint32_t i = 0; i < run; ++i) {
                    dst[out_pos++] = pixel0;
                    dst[out_pos++] = pixel1;
                }
            } else {
                uint32_t bytes = run * 2;
                if (in_pos + bytes > end || out_pos + bytes > expected_pixels * 2) {
                    return false;
                }
                memcpy(dst + out_pos, encoded + in_pos, bytes);
                in_pos += bytes;
                out_pos += bytes;
            }
        }
        return out_pos == expected_pixels * 2;
    };

    auto decode_rle_into = [&](const FrameEntry& entry, uint32_t rel_offset, uint8_t* dst, uint32_t expected_pixels) -> bool {
        if (rel_offset > entry.size) {
            return false;
        }
        uint32_t encoded_size = entry.size - rel_offset;
        auto* encoded = static_cast<uint8_t*>(heap_caps_malloc(encoded_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (encoded == nullptr) {
            encoded = static_cast<uint8_t*>(heap_caps_malloc(encoded_size, MALLOC_CAP_8BIT));
        }
        if (encoded == nullptr) {
            return false;
        }
        bool ok = read_frame(entry, rel_offset, encoded, encoded_size) &&
            decode_rle_buffer(encoded, encoded_size, dst, expected_pixels);
        heap_caps_free(encoded);
        return ok;
    };

    auto read_palette = [&](const FrameEntry& entry, uint32_t* rel_offset, uint16_t** palette_out, uint16_t* palette_count_out) -> bool {
        uint16_t palette_count = 0;
        if (*rel_offset + sizeof(uint16_t) > entry.size || !read_frame(entry, *rel_offset, &palette_count, sizeof(uint16_t))) {
            return false;
        }
        *rel_offset += sizeof(uint16_t);
        if (palette_count == 0 || palette_count > 256) {
            return false;
        }
        auto* palette = static_cast<uint16_t*>(heap_caps_malloc(palette_count * sizeof(uint16_t), MALLOC_CAP_8BIT));
        if (palette == nullptr) {
            return false;
        }
        if (*rel_offset + palette_count * sizeof(uint16_t) > entry.size || !read_frame(entry, *rel_offset, palette, palette_count * sizeof(uint16_t))) {
            heap_caps_free(palette);
            return false;
        }
        *rel_offset += palette_count * sizeof(uint16_t);
        *palette_out = palette;
        *palette_count_out = palette_count;
        return true;
    };

    auto decode_indexed = [&](const FrameEntry& entry, uint32_t rel_offset, uint8_t* dst, uint32_t pixel_count) -> bool {
        uint16_t* palette = nullptr;
        uint16_t palette_count = 0;
        if (!read_palette(entry, &rel_offset, &palette, &palette_count)) {
            return false;
        }
        auto* indices = static_cast<uint8_t*>(heap_caps_malloc(pixel_count, MALLOC_CAP_8BIT));
        if (indices == nullptr) {
            heap_caps_free(palette);
            return false;
        }
        bool ok = decode_heatshrink(entry, rel_offset, indices, pixel_count);
        uint32_t out_pos = 0;
        for (uint32_t i = 0; ok && i < pixel_count; ++i) {
            if (indices[i] >= palette_count) {
                ok = false;
                break;
            }
            uint16_t value = palette[indices[i]];
            dst[out_pos++] = value & 0xFF;
            dst[out_pos++] = value >> 8;
        }
        heap_caps_free(indices);
        heap_caps_free(palette);
        return ok;
    };

    auto decode_indexed_raw = [&](const FrameEntry& entry, uint32_t rel_offset, uint8_t* dst, uint32_t pixel_count) -> bool {
        uint16_t* palette = nullptr;
        uint16_t palette_count = 0;
        if (!read_palette(entry, &rel_offset, &palette, &palette_count)) {
            return false;
        }
        if (entry.size - rel_offset != pixel_count) {
            heap_caps_free(palette);
            return false;
        }
        auto* indices = static_cast<uint8_t*>(heap_caps_malloc(pixel_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (indices == nullptr) {
            indices = static_cast<uint8_t*>(heap_caps_malloc(pixel_count, MALLOC_CAP_8BIT));
        }
        if (indices == nullptr) {
            heap_caps_free(palette);
            return false;
        }
        bool ok = read_frame(entry, rel_offset, indices, pixel_count);
        uint32_t out_pos = 0;
        for (uint32_t i = 0; ok && i < pixel_count; ++i) {
            uint8_t index = indices[i];
            if (index >= palette_count) {
                ok = false;
                break;
            }
            uint16_t value = palette[index];
            dst[out_pos++] = value & 0xFF;
            dst[out_pos++] = value >> 8;
        }
        heap_caps_free(indices);
        heap_caps_free(palette);
        return ok;
    };

    auto apply_rect = [&](const DeltaHeader& delta, const uint8_t* rect) {
        uint32_t src_pos = 0;
        for (uint16_t y = delta.y; y < delta.y + delta.h; ++y) {
            uint32_t dst_pos = (static_cast<uint32_t>(y) * width_ + delta.x) * 2;
            memcpy(frame_buffer_ + dst_pos, rect + src_pos, static_cast<uint32_t>(delta.w) * 2);
            src_pos += static_cast<uint32_t>(delta.w) * 2;
        }
    };

    bool frame_drawn_directly = false;
    auto decode_frame = [&](const FrameEntry& entry) -> bool {
        frame_drawn_directly = false;
        if (entry.encoding == kEncodingRleRgb565) {
            bool ok = decode_rle_into(entry, 0, frame_buffer_, static_cast<uint32_t>(width_) * height_);
            if (ok) {
                SetDirtyFullFrame();
            }
            return ok;
        }
        if (entry.encoding == kEncodingHeatshrinkRgb565) {
            bool ok = decode_heatshrink(entry, 0, frame_buffer_, frame_bytes_);
            if (ok) {
                SetDirtyFullFrame();
            }
            return ok;
        }
        if (entry.encoding == kEncodingIndexedHeatshrink) {
            bool ok = decode_indexed(entry, 0, frame_buffer_, static_cast<uint32_t>(width_) * height_);
            if (ok) {
                SetDirtyFullFrame();
            }
            return ok;
        }
        if (entry.encoding == kEncodingIndexedRaw) {
            if (UseDirectPanelDraw()) {
                bool ok = decode_indexed_raw(entry, 0, frame_buffer_, static_cast<uint32_t>(width_) * height_);
                if (ok) {
                    SetDirtyFullFrame();
                }
                return ok;
            }
            bool ok = decode_indexed_raw(entry, 0, frame_buffer_, static_cast<uint32_t>(width_) * height_);
            if (ok) {
                SetDirtyFullFrame();
            }
            return ok;
        }

        DeltaHeader delta{};
        if (entry.size < sizeof(delta) || !read_frame(entry, 0, &delta, sizeof(delta))) {
            return false;
        }
        if (delta.x > width_ || delta.y > height_ || delta.w > width_ || delta.h > height_ ||
            delta.x + delta.w > width_ || delta.y + delta.h > height_) {
            return false;
        }
        if (delta.w == 0 || delta.h == 0) {
            dirty_w_ = 0;
            dirty_h_ = 0;
            return true;
        }
        uint32_t rect_pixels = static_cast<uint32_t>(delta.w) * delta.h;
        uint32_t rect_bytes = rect_pixels * 2;
        auto* rect = static_cast<uint8_t*>(heap_caps_malloc(rect_bytes, MALLOC_CAP_8BIT));
        if (rect == nullptr) {
            return false;
        }
        bool ok = false;
        if (entry.encoding == kEncodingDeltaRleRgb565) {
            ok = decode_rle_into(entry, sizeof(delta), rect, rect_pixels);
        } else if (entry.encoding == kEncodingDeltaHeatshrinkRgb565) {
            ok = decode_heatshrink(entry, sizeof(delta), rect, rect_bytes);
        } else if (entry.encoding == kEncodingDeltaIndexedHeatshrink) {
            ok = decode_indexed(entry, sizeof(delta), rect, rect_pixels);
        } else if (entry.encoding == kEncodingDeltaIndexedRaw) {
            if (UseDirectPanelDraw()) {
                ok = decode_indexed_raw(entry, sizeof(delta), rect, rect_pixels);
                if (ok) {
                    apply_rect(delta, rect);
                    dirty_x_ = delta.x;
                    dirty_y_ = delta.y;
                    dirty_w_ = delta.w;
                    dirty_h_ = delta.h;
                }
                heap_caps_free(rect);
                return ok;
            }
            ok = decode_indexed_raw(entry, sizeof(delta), rect, rect_pixels);
        }
        if (ok) {
            apply_rect(delta, rect);
            dirty_x_ = delta.x;
            dirty_y_ = delta.y;
            dirty_w_ = delta.w;
            dirty_h_ = delta.h;
        }
        heap_caps_free(rect);
        return ok;
    };

    auto wait_until_interval = [&](int64_t start_ms, uint16_t wait_ms) {
        int64_t elapsed = esp_timer_get_time() / 1000 - start_ms;
        if (elapsed < wait_ms) {
            vTaskDelay(pdMS_TO_TICKS(wait_ms - elapsed));
        }
        return elapsed;
    };

    while (!stop_requested_) {
        for (uint16_t i = 0; i < header.frame_count && !stop_requested_; ++i) {
            int64_t frame_start = esp_timer_get_time() / 1000;
            if (!decode_frame(frames[i])) {
                ESP_LOGW(TAG, "Failed to decode GIFP frame %u: offset=%lu size=%lu delay=%u encoding=%u",
                    i, static_cast<unsigned long>(frames[i].offset), static_cast<unsigned long>(frames[i].size), frames[i].delay_ms, frames[i].encoding);
                heap_caps_free(frames);
                return false;
            }
            if (!frame_drawn_directly) {
                DrawFrame();
            }
            uint16_t wait_ms = frames[i].delay_ms < 20 ? 20 : frames[i].delay_ms;
            int64_t elapsed = esp_timer_get_time() / 1000 - frame_start;
            if (i == 0 || i % 30 == 0) {
                ESP_LOGI(TAG, "GIFP frame %u/%u elapsed=%dms delay=%ums synth=%u encoding=%u size=%lu dirty=%ux%u+%u+%u",
                    i + 1, header.frame_count, static_cast<int>(elapsed), wait_ms,
                    frames[i].synthetic_count, frames[i].encoding, static_cast<unsigned long>(frames[i].size),
                    dirty_w_, dirty_h_, dirty_x_, dirty_y_);
            }
            wait_until_interval(frame_start, wait_ms);
            for (uint8_t phase = 0; phase < frames[i].synthetic_count && !stop_requested_; ++phase) {
                vTaskDelay(pdMS_TO_TICKS(wait_ms));
            }
        }
    }

    heap_caps_free(frames);
    return true;
}

bool GifpPlayer::DrawIndexedRawFrame(const uint16_t* palette, uint16_t palette_count, const uint8_t* indices, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (palette == nullptr || indices == nullptr || display_buffer_ == nullptr || w == 0 || h == 0) {
        return false;
    }
    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "Timed out waiting for LVGL lock");
        return false;
    }
    if (layer_ != nullptr) {
        DeleteLvglObjects();
    }
    esp_err_t err = ESP_OK;
    constexpr uint16_t kDirectDrawRows = 80;
    const uint32_t row_bytes = static_cast<uint32_t>(w) * 2;
    int64_t total_convert_ms = 0;
    int64_t total_draw_ms = 0;
    for (uint16_t rel_y = 0; rel_y < h; rel_y += kDirectDrawRows) {
        uint16_t rows = std::min<uint16_t>(kDirectDrawRows, h - rel_y);
        int64_t convert_start = esp_timer_get_time() / 1000;
        for (uint16_t row = 0; row < rows; ++row) {
            const uint32_t src_row = static_cast<uint32_t>(rel_y + row) * w;
            uint8_t* dst = display_buffer_ + static_cast<uint32_t>(row) * row_bytes;
            for (uint16_t col = 0; col < w; ++col) {
                uint8_t index = indices[src_row + col];
                if (index >= palette_count) {
                    lvgl_port_unlock();
                    return false;
                }
                uint16_t value = palette[index];
                dst[col * 2] = value >> 8;
                dst[col * 2 + 1] = value & 0xFF;
            }
        }
        total_convert_ms += esp_timer_get_time() / 1000 - convert_start;
        int64_t draw_start = esp_timer_get_time() / 1000;
        err = esp_lcd_panel_draw_bitmap(panel_, x, y + rel_y, x + w, y + rel_y + rows, display_buffer_);
        total_draw_ms += esp_timer_get_time() / 1000 - draw_start;
        if (err != ESP_OK) {
            break;
        }
        if (!WaitPanelTransferDone()) {
            err = ESP_FAIL;
            break;
        }
    }
    lvgl_port_unlock();
    if (total_convert_ms + total_draw_ms > 80) {
        ESP_LOGI(TAG, "GIFP indexed draw: %ux%u convert=%dms draw=%dms", w, h,
            static_cast<int>(total_convert_ms), static_cast<int>(total_draw_ms));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Direct indexed GIFP panel draw failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool GifpPlayer::WaitPanelTransferDone() {
    if (panel_io_ == nullptr) {
        return true;
    }
    esp_err_t err = esp_lcd_panel_io_tx_param(panel_io_, -1, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wait panel transfer failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void GifpPlayer::DrawFrame() {
    if (frame_buffer_ == nullptr || display_buffer_ == nullptr) {
        return;
    }
    if (UseDirectPanelDraw()) {
        if (dirty_w_ == 0 || dirty_h_ == 0) {
            return;
        }
        uint8_t scale = DirectPanelScale();
        if (scale > 1) {
            SetDirtyFullFrame();
        }
        if (!lvgl_port_lock(100)) {
            ESP_LOGW(TAG, "Timed out waiting for LVGL lock");
            return;
        }
        if (layer_ != nullptr) {
            DeleteLvglObjects();
        }
        esp_err_t err = ESP_OK;
        const uint32_t row_bytes = static_cast<uint32_t>(dirty_w_) * scale * 2;
        const uint16_t source_rows_per_chunk = std::max<uint16_t>(1, kDirectDrawRows / scale);
        for (uint16_t y = dirty_y_; y < dirty_y_ + dirty_h_; y += source_rows_per_chunk) {
            uint16_t source_rows = std::min<uint16_t>(source_rows_per_chunk, dirty_y_ + dirty_h_ - y);
            for (uint16_t row_index = 0; row_index < source_rows; ++row_index) {
                const uint32_t src_pos = (static_cast<uint32_t>(y + row_index) * width_ + dirty_x_) * 2;
                if (scale == 2 && dirty_x_ == 0 && dirty_w_ == width_) {
                    uint8_t* dst0 = display_buffer_ + static_cast<uint32_t>(row_index) * 2 * row_bytes;
                    uint8_t* dst1 = dst0 + row_bytes;
                    uint16_t src_y = y + row_index;
                    uint16_t y_up = src_y == 0 ? src_y : src_y - 1;
                    uint16_t y_down = src_y + 1 >= height_ ? src_y : src_y + 1;
                    for (uint16_t x = 0; x < width_; ++x) {
                        uint16_t x_left = x == 0 ? x : x - 1;
                        uint16_t x_right = x + 1 >= width_ ? x : x + 1;
                        uint16_t up = ReadRgb565Le(frame_buffer_, width_, x, y_up);
                        uint16_t left = ReadRgb565Le(frame_buffer_, width_, x_left, src_y);
                        uint16_t center = ReadRgb565Le(frame_buffer_, width_, x, src_y);
                        uint16_t right = ReadRgb565Le(frame_buffer_, width_, x_right, src_y);
                        uint16_t down = ReadRgb565Le(frame_buffer_, width_, x, y_down);
                        uint16_t sharp = SharpenRgb565(center, up, left, right, down);
                        uint32_t out = static_cast<uint32_t>(x) * 2;
                        WriteRgb565Be(dst0, out, sharp);
                        WriteRgb565Be(dst0, out + 1, sharp);
                        WriteRgb565Be(dst1, out, sharp);
                        WriteRgb565Be(dst1, out + 1, sharp);
                    }
                } else {
                    for (uint8_t sy = 0; sy < scale; ++sy) {
                        uint8_t* dst = display_buffer_ + (static_cast<uint32_t>(row_index) * scale + sy) * row_bytes;
                        for (uint16_t x = 0; x < dirty_w_; ++x) {
                            uint32_t src_x = src_pos + static_cast<uint32_t>(x) * 2;
                            for (uint8_t sx = 0; sx < scale; ++sx) {
                                uint32_t dst_x = (static_cast<uint32_t>(x) * scale + sx) * 2;
                                dst[dst_x] = frame_buffer_[src_x + 1];
                                dst[dst_x + 1] = frame_buffer_[src_x];
                            }
                        }
                    }
                }
            }
            uint16_t dst_x = dirty_x_ * scale;
            uint16_t dst_y = y * scale;
            err = esp_lcd_panel_draw_bitmap(panel_, dst_x, dst_y, dst_x + dirty_w_ * scale, dst_y + source_rows * scale, display_buffer_);
            if (err != ESP_OK) {
                break;
            }
            if (!WaitPanelTransferDone()) {
                err = ESP_FAIL;
                break;
            }
        }
        lvgl_port_unlock();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Direct GIFP panel draw failed: %s", esp_err_to_name(err));
        }
        return;
    }
    if (transparent_color_key_) {
        auto* alpha = display_buffer_ + frame_bytes_;
        uint32_t pixels = static_cast<uint32_t>(width_) * height_;
        for (uint32_t i = 0; i < pixels; ++i) {
            uint8_t lo = frame_buffer_[i * 2];
            uint8_t hi = frame_buffer_[i * 2 + 1];
            uint16_t value = lo | (static_cast<uint16_t>(hi) << 8);
            display_buffer_[i * 2] = lo;
            display_buffer_[i * 2 + 1] = hi;
            alpha[i] = value == kTransparentRgb565 ? LV_OPA_TRANSP : LV_OPA_COVER;
        }
    } else {
        memcpy(display_buffer_, frame_buffer_, frame_bytes_);
    }
    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "Timed out waiting for LVGL lock");
        return;
    }
    if (image_dsc_.data != nullptr) {
        lv_image_cache_drop(&image_dsc_);
    }
    CreateOrUpdateLvglObjects();
    lvgl_port_unlock();
}

bool GifpPlayer::UseDirectPanelDraw() const {
    return panel_ != nullptr && DirectPanelScale() > 0 && !transparent_color_key_;
}

uint8_t GifpPlayer::DirectPanelScale() const {
    if (width_ == DISPLAY_WIDTH && height_ == DISPLAY_HEIGHT) {
        return 1;
    }
    if (width_ * 2 == DISPLAY_WIDTH && height_ * 2 == DISPLAY_HEIGHT) {
        return 2;
    }
    return 0;
}

void GifpPlayer::SetDirtyFullFrame() {
    dirty_x_ = 0;
    dirty_y_ = 0;
    dirty_w_ = width_;
    dirty_h_ = height_;
}

void GifpPlayer::CreateOrUpdateLvglObjects() {
    if (display_buffer_ == nullptr || width_ == 0 || height_ == 0) {
        return;
    }
    if (layer_ == nullptr) {
        layer_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(layer_);
        lv_obj_set_size(layer_, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        lv_obj_set_style_bg_opa(layer_, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(layer_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_center(layer_);

        image_ = lv_image_create(layer_);
        lv_obj_center(image_);
        ESP_LOGI(TAG, "Created GIFP LVGL layer %ux%u on top layer", width_, height_);
    }

    const lv_color_format_t color_format = transparent_color_key_ ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    const uint32_t data_size = frame_bytes_ + (transparent_color_key_ ? static_cast<uint32_t>(width_) * height_ : 0);
    const bool source_changed = image_dsc_.data != display_buffer_ ||
        image_dsc_.header.cf != color_format ||
        image_dsc_.header.w != width_ ||
        image_dsc_.header.h != height_ ||
        image_dsc_.data_size != data_size;

    if (source_changed) {
        if (image_dsc_.data != nullptr) {
            lv_image_cache_drop(&image_dsc_);
        }
        memset(&image_dsc_, 0, sizeof(image_dsc_));
        image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        image_dsc_.header.cf = color_format;
        image_dsc_.header.w = width_;
        image_dsc_.header.h = height_;
        image_dsc_.header.stride = width_ * 2;
        image_dsc_.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
        image_dsc_.data_size = data_size;
        image_dsc_.data = display_buffer_;

        lv_image_set_src(image_, &image_dsc_);
        if (width_ == 64 && height_ == 64) {
            lv_image_set_scale(image_, 768);
        } else if (width_ != DISPLAY_WIDTH || height_ != DISPLAY_HEIGHT) {
            uint32_t scale_x = static_cast<uint32_t>(DISPLAY_WIDTH) * 256 / width_;
            uint32_t scale_y = static_cast<uint32_t>(DISPLAY_HEIGHT) * 256 / height_;
            lv_image_set_scale(image_, std::min<uint32_t>(scale_x, scale_y));
        } else {
            lv_image_set_scale(image_, 256);
        }
        lv_obj_center(image_);
    }
    lv_obj_move_foreground(layer_);
    lv_obj_invalidate(image_);
    lv_obj_invalidate(layer_);
}

void GifpPlayer::DeleteLvglObjects() {
    if (layer_ != nullptr) {
        lv_obj_delete(layer_);
        layer_ = nullptr;
        image_ = nullptr;
    }
    memset(&image_dsc_, 0, sizeof(image_dsc_));
}
