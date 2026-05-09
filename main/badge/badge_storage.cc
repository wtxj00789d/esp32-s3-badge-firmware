#include "badge_storage.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

namespace {
constexpr const char* TAG = "BadgeStorage";
constexpr const char* kMountPoint = "/sdcard";
constexpr const char* kBadgeDir = "/sdcard/BADGE";
constexpr const char* kRecordingDir = "/sdcard/REC";

constexpr gpio_num_t kSdClk = GPIO_NUM_17;
constexpr gpio_num_t kSdCmd = GPIO_NUM_18;
constexpr gpio_num_t kSdD0 = GPIO_NUM_21;
constexpr gpio_num_t kSdD3 = GPIO_NUM_13;

bool IsBadgeBwpName(const char* name)
{
    if (std::strlen(name) != 7) {
        return false;
    }
    if (!std::isdigit(static_cast<unsigned char>(name[0])) ||
        !std::isdigit(static_cast<unsigned char>(name[1])) ||
        !std::isdigit(static_cast<unsigned char>(name[2])) ||
        name[3] != '.') {
        return false;
    }
    return std::toupper(static_cast<unsigned char>(name[4])) == 'B' &&
        std::toupper(static_cast<unsigned char>(name[5])) == 'W' &&
        std::toupper(static_cast<unsigned char>(name[6])) == 'P';
}

std::string BasenameFromBwpName(const char* name)
{
    std::string basename(name, 3);
    std::transform(basename.begin(), basename.end(), basename.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return basename;
}
}

bool BadgeStorage::Mount()
{
    if (mounted_) {
        return true;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = 20000;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = kSdClk;
    slot_config.cmd = kSdCmd;
    slot_config.d0 = kSdD0;
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = kSdD3;
    slot_config.d4 = GPIO_NUM_NC;
    slot_config.d5 = GPIO_NUM_NC;
    slot_config.d6 = GPIO_NUM_NC;
    slot_config.d7 = GPIO_NUM_NC;
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t* card = nullptr;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted");
    sdmmc_card_print_info(stdout, card);

    if (!EnsureDirectory(kBadgeDir) || !EnsureDirectory(kRecordingDir)) {
        ESP_LOGW(TAG, "Failed to prepare badge directories");
        esp_vfs_fat_sdcard_unmount(kMountPoint, card);
        media_.clear();
        return false;
    }

    ScanMedia();
    mounted_ = true;
    return true;
}

bool BadgeStorage::EnsureDirectory(const char* path)
{
    struct stat st {};
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return true;
        }
        ESP_LOGW(TAG, "Path exists but is not a directory: %s", path);
        return false;
    }

    if (mkdir(path, 0775) == 0) {
        ESP_LOGI(TAG, "Created directory: %s", path);
        return true;
    }

    if (errno == EEXIST) {
        return true;
    }

    ESP_LOGW(TAG, "Failed to create directory %s: errno=%d", path, errno);
    return false;
}

void BadgeStorage::ScanMedia()
{
    media_.clear();

    DIR* dir = opendir(kBadgeDir);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "Failed to open media directory: %s", kBadgeDir);
        return;
    }

    while (dirent* entry = readdir(dir)) {
        if (!IsBadgeBwpName(entry->d_name)) {
            continue;
        }

        BadgeMediaItem item;
        item.basename = BasenameFromBwpName(entry->d_name);
        item.bwp_path = std::string(kBadgeDir) + "/" + entry->d_name;
        item.wav_path = std::string(kBadgeDir) + "/" + item.basename + ".WAV";
        item.has_wav = FileExists(item.wav_path);
        media_.push_back(item);
    }

    closedir(dir);
    std::sort(media_.begin(), media_.end(), [](const BadgeMediaItem& lhs, const BadgeMediaItem& rhs) {
        return lhs.basename < rhs.basename;
    });

    ESP_LOGI(TAG, "Found %u badge media item(s)", static_cast<unsigned>(media_.size()));
}

bool BadgeStorage::FileExists(const std::string& path) const
{
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string BadgeStorage::NextRecordingPath(int number) const
{
    char path[32];
    std::snprintf(path, sizeof(path), "%s/REC%04d.WAV", kRecordingDir, number);
    return path;
}
