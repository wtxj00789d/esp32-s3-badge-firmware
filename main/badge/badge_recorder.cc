#include "badge_recorder.h"

#include <cstring>
#include <vector>

#include <esp_log.h>

namespace {
constexpr const char* TAG = "BadgeRecorder";
constexpr uint16_t kPcmFormat = 1;
constexpr uint16_t kChannels = 1;
constexpr uint32_t kSampleRate = 24000;
constexpr uint16_t kBitsPerSample = 16;
constexpr uint16_t kBlockAlign = kChannels * kBitsPerSample / 8;
constexpr uint32_t kByteRate = kSampleRate * kBlockAlign;
constexpr size_t kChunkSamples = 240;

void WriteLe16(uint8_t* dest, uint16_t value)
{
    dest[0] = static_cast<uint8_t>(value & 0xff);
    dest[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void WriteLe32(uint8_t* dest, uint32_t value)
{
    dest[0] = static_cast<uint8_t>(value & 0xff);
    dest[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    dest[2] = static_cast<uint8_t>((value >> 16) & 0xff);
    dest[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}
}

BadgeRecorder::BadgeRecorder(AudioCodec& audio)
    : audio_(audio)
{
}

BadgeRecorder::~BadgeRecorder()
{
    Stop();
}

bool BadgeRecorder::Start(const char* path)
{
    if (recording_) {
        ESP_LOGW(TAG, "Recorder is already running");
        return false;
    }

    file_ = std::fopen(path, "wb+");
    if (file_ == nullptr) {
        ESP_LOGW(TAG, "Failed to create recording: %s", path);
        return false;
    }

    bytes_written_ = 0;
    if (!WritePlaceholderHeader()) {
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }

    stop_requested_ = false;
    recording_ = true;
    BaseType_t ok = xTaskCreate(
        &BadgeRecorder::TaskEntry,
        "badge_rec",
        4096,
        this,
        5,
        &task_);
    if (ok != pdPASS) {
        recording_ = false;
        std::fclose(file_);
        file_ = nullptr;
        task_ = nullptr;
        ESP_LOGE(TAG, "Failed to create recorder task");
        return false;
    }

    ESP_LOGI(TAG, "Recording started: %s", path);
    return true;
}

void BadgeRecorder::Stop()
{
    if (!recording_) {
        return;
    }

    stop_requested_ = true;
    while (recording_) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void BadgeRecorder::TaskEntry(void* arg)
{
    static_cast<BadgeRecorder*>(arg)->Run();
    vTaskDelete(nullptr);
}

void BadgeRecorder::Run()
{
    audio_.EnableInput(true);

    std::vector<int16_t> samples(kChunkSamples);
    while (!stop_requested_) {
        if (audio_.InputData(samples)) {
            const size_t bytes = samples.size() * sizeof(int16_t);
            if (std::fwrite(samples.data(), 1, bytes, file_) != bytes) {
                ESP_LOGE(TAG, "Failed to write recording data");
                break;
            }
            bytes_written_ += static_cast<uint32_t>(bytes);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    audio_.EnableInput(false);
    FinalizeHeader();
    std::fclose(file_);
    file_ = nullptr;
    task_ = nullptr;
    recording_ = false;
    ESP_LOGI(TAG, "Recording stopped (%lu bytes)", static_cast<unsigned long>(bytes_written_));
}

bool BadgeRecorder::WritePlaceholderHeader()
{
    uint8_t header[44] = {};
    std::memcpy(header, "RIFF", 4);
    WriteLe32(header + 4, 36);
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    WriteLe32(header + 16, 16);
    WriteLe16(header + 20, kPcmFormat);
    WriteLe16(header + 22, kChannels);
    WriteLe32(header + 24, kSampleRate);
    WriteLe32(header + 28, kByteRate);
    WriteLe16(header + 32, kBlockAlign);
    WriteLe16(header + 34, kBitsPerSample);
    std::memcpy(header + 36, "data", 4);
    WriteLe32(header + 40, 0);
    return std::fwrite(header, 1, sizeof(header), file_) == sizeof(header);
}

void BadgeRecorder::FinalizeHeader()
{
    if (file_ == nullptr) {
        return;
    }

    std::fflush(file_);
    std::fseek(file_, 4, SEEK_SET);
    uint8_t value[4];
    WriteLe32(value, 36 + bytes_written_);
    std::fwrite(value, 1, sizeof(value), file_);
    std::fseek(file_, 40, SEEK_SET);
    WriteLe32(value, bytes_written_);
    std::fwrite(value, 1, sizeof(value), file_);
    std::fflush(file_);
}
