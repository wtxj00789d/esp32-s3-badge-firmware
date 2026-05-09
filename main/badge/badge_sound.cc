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
    if (path == nullptr) {
        ESP_LOGW(TAG, "No WAV path provided");
        return false;
    }

    path_ = path;
    source_ = Source::File;
    pcm_samples_ = nullptr;
    pcm_sample_count_ = 0;
    return StartTask();
}

bool BadgeSoundPlayer::PlayPcm(const int16_t* samples, size_t sample_count)
{
    if (playing_) {
        ESP_LOGW(TAG, "Sound is already playing");
        return false;
    }
    if (samples == nullptr || sample_count == 0) {
        ESP_LOGW(TAG, "No embedded PCM sound data");
        return false;
    }

    source_ = Source::Pcm;
    path_.clear();
    pcm_samples_ = samples;
    pcm_sample_count_ = sample_count;
    return StartTask();
}

void BadgeSoundPlayer::Stop()
{
    stop_requested_ = true;
    while (playing_ && task_ != nullptr && xTaskGetCurrentTaskHandle() != task_) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void BadgeSoundPlayer::TaskEntry(void* arg)
{
    static_cast<BadgeSoundPlayer*>(arg)->Run();
    vTaskDelete(nullptr);
}

bool BadgeSoundPlayer::StartTask()
{
    playing_ = true;
    stop_requested_ = false;
    BaseType_t ok = xTaskCreate(
        &BadgeSoundPlayer::TaskEntry,
        "badge_sound",
        4096,
        this,
        5,
        &task_);
    if (ok != pdPASS) {
        task_ = nullptr;
        playing_ = false;
        source_ = Source::None;
        ESP_LOGE(TAG, "Failed to create sound task");
        return false;
    }
    return true;
}

void BadgeSoundPlayer::Run()
{
    audio_.EnableOutput(true);

    switch (source_) {
    case Source::File:
        RunFile();
        break;
    case Source::Pcm:
        RunPcm();
        break;
    case Source::None:
        break;
    }

    audio_.EnableOutput(false);
    source_ = Source::None;
    task_ = nullptr;
    playing_ = false;
}

void BadgeSoundPlayer::RunFile()
{
    FILE* file = std::fopen(path_.c_str(), "rb");
    if (file == nullptr) {
        ESP_LOGW(TAG, "Failed to open sound file: %s", path_.c_str());
        return;
    }

    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    if (!ParseWav(file, data_offset, data_size) ||
        std::fseek(file, static_cast<long>(data_offset), SEEK_SET) != 0) {
        std::fclose(file);
        return;
    }

    ESP_LOGI(TAG, "Play WAV: %s (%lu bytes)", path_.c_str(), static_cast<unsigned long>(data_size));
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

    std::fclose(file);
}

void BadgeSoundPlayer::RunPcm()
{
    ESP_LOGI(TAG, "Play embedded PCM sound (%u samples)", static_cast<unsigned>(pcm_sample_count_));
    std::vector<int16_t> chunk;
    chunk.reserve(kChunkSamples);
    size_t offset = 0;
    while (offset < pcm_sample_count_ && !stop_requested_) {
        const size_t count = std::min<size_t>(kChunkSamples, pcm_sample_count_ - offset);
        chunk.assign(pcm_samples_ + offset, pcm_samples_ + offset + count);
        audio_.OutputData(chunk);
        offset += count;
    }
}
