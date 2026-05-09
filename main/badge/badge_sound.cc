#include "badge_sound.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <esp_log.h>

namespace {
constexpr const char* TAG = "BadgeSound";
constexpr uint16_t kPcmFormat = 1;
constexpr uint16_t kChannels = 1;
constexpr uint32_t kSampleRate = 24000;
constexpr uint16_t kBitsPerSample = 16;
constexpr size_t kChunkSamples = 240;

uint16_t ReadLe16(const uint8_t* data)
{
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

bool ReadExact(FILE* file, void* dest, size_t bytes)
{
    return std::fread(dest, 1, bytes, file) == bytes;
}

bool SkipBytes(FILE* file, uint32_t bytes)
{
    return std::fseek(file, static_cast<long>(bytes), SEEK_CUR) == 0;
}

bool ParseWav(FILE* file, uint32_t& data_offset, uint32_t& data_size)
{
    uint8_t riff[12];
    if (!ReadExact(file, riff, sizeof(riff)) ||
        std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0) {
        ESP_LOGW(TAG, "Not a RIFF/WAVE file");
        return false;
    }

    bool fmt_ok = false;
    bool data_ok = false;
    while (!data_ok) {
        uint8_t header[8];
        if (!ReadExact(file, header, sizeof(header))) {
            break;
        }

        const uint32_t chunk_size = ReadLe32(header + 4);
        const long payload_offset = std::ftell(file);
        if (payload_offset < 0) {
            return false;
        }

        if (std::memcmp(header, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                ESP_LOGW(TAG, "WAV fmt chunk is too small");
                return false;
            }

            uint8_t fmt[16];
            if (!ReadExact(file, fmt, sizeof(fmt))) {
                return false;
            }

            const uint16_t audio_format = ReadLe16(fmt);
            const uint16_t channels = ReadLe16(fmt + 2);
            const uint32_t sample_rate = ReadLe32(fmt + 4);
            const uint16_t bits_per_sample = ReadLe16(fmt + 14);
            fmt_ok = audio_format == kPcmFormat &&
                channels == kChannels &&
                sample_rate == kSampleRate &&
                bits_per_sample == kBitsPerSample;
            if (!fmt_ok) {
                ESP_LOGW(TAG, "Unsupported WAV format: format=%u channels=%u rate=%lu bits=%u",
                    audio_format, channels, static_cast<unsigned long>(sample_rate), bits_per_sample);
                return false;
            }

            if (chunk_size > sizeof(fmt) && !SkipBytes(file, chunk_size - sizeof(fmt))) {
                return false;
            }
        } else if (std::memcmp(header, "data", 4) == 0) {
            data_offset = static_cast<uint32_t>(payload_offset);
            data_size = chunk_size;
            data_ok = true;
            if (!SkipBytes(file, chunk_size)) {
                return false;
            }
        } else if (!SkipBytes(file, chunk_size)) {
            return false;
        }

        if ((chunk_size & 1) != 0 && !SkipBytes(file, 1)) {
            return false;
        }
    }

    if (!fmt_ok || !data_ok || data_size == 0) {
        ESP_LOGW(TAG, "WAV is missing required fmt/data chunks");
        return false;
    }
    return true;
}
}

BadgeSoundPlayer::BadgeSoundPlayer(AudioCodec& audio)
    : audio_(audio)
{
}

bool BadgeSoundPlayer::Play(const char* path)
{
    if (playing_) {
        ESP_LOGW(TAG, "Sound is already playing");
        return false;
    }

    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGW(TAG, "Failed to open sound file: %s", path);
        return false;
    }

    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    if (!ParseWav(file, data_offset, data_size) ||
        std::fseek(file, static_cast<long>(data_offset), SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }

    ESP_LOGI(TAG, "Play WAV: %s (%lu bytes)", path, static_cast<unsigned long>(data_size));
    playing_ = true;
    stop_requested_ = false;
    audio_.EnableOutput(true);

    std::vector<int16_t> samples(kChunkSamples);
    uint32_t remaining = data_size;
    while (remaining > 0 && !stop_requested_) {
        const size_t wanted = std::min<size_t>(samples.size() * sizeof(int16_t), remaining);
        const size_t bytes_read = std::fread(samples.data(), 1, wanted, file);
        if (bytes_read == 0) {
            break;
        }

        samples.resize(bytes_read / sizeof(int16_t));
        if (!samples.empty()) {
            audio_.OutputData(samples);
        }
        samples.resize(kChunkSamples);
        remaining -= static_cast<uint32_t>(bytes_read);
    }

    audio_.EnableOutput(false);
    playing_ = false;
    std::fclose(file);
    return true;
}

void BadgeSoundPlayer::Stop()
{
    stop_requested_ = true;
}
