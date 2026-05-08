#include "sd_wallpaper_manager.h"
#include "config.h"
#include "settings.h"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>

#define TAG "SdWallpaper"

namespace {
bool EndsWithGifp(const char* name) {
    size_t len = strlen(name);
    return len > 5 && strcasecmp(name + len - 5, ".gifp") == 0;
}

constexpr size_t kWallpaperPsramReserve = 768 * 1024;
} // namespace

SdWallpaperManager::SdWallpaperManager(esp_io_expander_handle_t io_expander, esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io)
    : io_expander_(io_expander), panel_(panel), panel_io_(panel_io) {
    player_.SetPanelHandle(panel_);
    player_.SetPanelIoHandle(panel_io_);
}

SdWallpaperManager::~SdWallpaperManager() {
    ShowDefaultPage();
    Unmount();
}

bool SdWallpaperManager::Initialize() {
    if (!Mount()) {
        return false;
    }
    ScanWallpapers();
    return true;
}

bool SdWallpaperManager::Mount() {
    if (mounted_) {
        return true;
    }

    if (io_expander_ != nullptr) {
        ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander_, TF_CARD_CS_EXPANDER_PIN, IO_EXPANDER_OUTPUT));
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_, TF_CARD_CS_EXPANDER_PIN, 1));
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = TF_CARD_PIN_NUM_CLK;
    slot_config.cmd = TF_CARD_PIN_NUM_CMD;
    slot_config.d0 = TF_CARD_PIN_NUM_D0;
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
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
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;
    mount_config.disk_status_check_enable = true;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card_);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return false;
    }

    mounted_ = true;
    mkdir(WALLPAPER_DIR, 0775);
    ESP_LOGI(TAG, "SD card mounted in 1-bit SDMMC mode");
    sdmmc_card_print_info(stdout, card_);
    return true;
}

void SdWallpaperManager::Unmount() {
    if (mounted_) {
        esp_vfs_fat_sdcard_unmount("/sdcard", card_);
        mounted_ = false;
        card_ = nullptr;
    }
    if (io_expander_ != nullptr) {
        esp_io_expander_set_level(io_expander_, TF_CARD_CS_EXPANDER_PIN, 1);
    }
}

void SdWallpaperManager::ScanWallpapers() {
    wallpapers_.clear();
    ScanWallpaperDirectory(WALLPAPER_DIR);
    ScanWallpaperDirectory(WALLPAPER_ROOT_DIR);
    std::sort(wallpapers_.begin(), wallpapers_.end());
    wallpapers_.erase(std::unique(wallpapers_.begin(), wallpapers_.end()), wallpapers_.end());
    current_index_ = wallpapers_.empty() ? -1 : 0;
    ESP_LOGI(TAG, "Found %u wallpaper GIFP file(s)", static_cast<unsigned>(wallpapers_.size()));
}

void SdWallpaperManager::ScanWallpaperDirectory(const char* dir_path) {
    DIR* dir = opendir(dir_path);
    if (dir == nullptr) {
        ESP_LOGI(TAG, "Wallpaper directory not open: %s", dir_path);
        return;
    }
    while (auto* entry = readdir(dir)) {
        ESP_LOGI(TAG, "SD entry in %s: %s", dir_path, entry->d_name);
        if (entry->d_name[0] == '.' || !EndsWithGifp(entry->d_name)) {
            continue;
        }
        std::string path = std::string(dir_path) + "/" + entry->d_name;
        wallpapers_.push_back(path);
        ESP_LOGI(TAG, "Wallpaper candidate: %s", path.c_str());
    }
    closedir(dir);
}

bool SdWallpaperManager::SwitchToNext() {
    if (!mounted_ && !Initialize()) {
        return false;
    }
    ScanWallpapers();
    if (wallpapers_.empty()) {
        return false;
    }
    Settings settings("wallpaper", true);
    std::string saved = settings.GetString("path");
    auto it = std::find(wallpapers_.begin(), wallpapers_.end(), saved);
    if (it != wallpapers_.end()) {
        current_index_ = static_cast<int>(std::distance(wallpapers_.begin(), it));
    }
    int next = (current_index_ + 1) % wallpapers_.size();
    return PlayIndex(next);
}

void SdWallpaperManager::ShowDefaultPage() {
    wallpaper_mode_ = false;
    player_.Stop();
    FreeCachedWallpaper();
}

bool SdWallpaperManager::RestoreSavedWallpaper() {
    if (!mounted_ && !Initialize()) {
        return false;
    }
    Settings settings("wallpaper", false);
    std::string saved = settings.GetString("path");
    if (saved.empty()) {
        return false;
    }
    ScanWallpapers();
    auto it = std::find(wallpapers_.begin(), wallpapers_.end(), saved);
    if (it == wallpapers_.end()) {
        return false;
    }
    return PlayIndex(static_cast<int>(std::distance(wallpapers_.begin(), it)));
}

void SdWallpaperManager::SaveCurrentPath() {
    if (current_index_ < 0 || current_index_ >= static_cast<int>(wallpapers_.size())) {
        return;
    }
    Settings settings("wallpaper", true);
    settings.SetString("path", wallpapers_[current_index_]);
}

bool SdWallpaperManager::PlayIndex(int index) {
    if (index < 0 || index >= static_cast<int>(wallpapers_.size())) {
        return false;
    }
    ESP_LOGI(TAG, "Play wallpaper %d/%u: %s", index + 1, static_cast<unsigned>(wallpapers_.size()), wallpapers_[index].c_str());
    player_.Stop();
    FreeCachedWallpaper();
    bool started = false;
    if (LoadWallpaperToPsram(wallpapers_[index])) {
        started = player_.StartMemory(cached_wallpaper_, cached_wallpaper_size_);
    } else {
        started = player_.StartFile(wallpapers_[index]);
    }
    if (!started) {
        FreeCachedWallpaper();
        return false;
    }
    current_index_ = index;
    wallpaper_mode_ = true;
    SaveCurrentPath();
    return true;
}

void SdWallpaperManager::FreeCachedWallpaper() {
    if (cached_wallpaper_ != nullptr) {
        heap_caps_free(cached_wallpaper_);
        cached_wallpaper_ = nullptr;
        cached_wallpaper_size_ = 0;
    }
}

bool SdWallpaperManager::LoadWallpaperToPsram(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
        return false;
    }
    size_t file_size = static_cast<size_t>(st.st_size);
    size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (largest_psram <= kWallpaperPsramReserve || file_size > largest_psram - kWallpaperPsramReserve) {
        ESP_LOGI(TAG, "Wallpaper too large for PSRAM cache: file=%u largest_psram=%u reserve=%u",
            static_cast<unsigned>(file_size), static_cast<unsigned>(largest_psram), static_cast<unsigned>(kWallpaperPsramReserve));
        return false;
    }

    auto* data = static_cast<uint8_t*>(heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data == nullptr) {
        ESP_LOGW(TAG, "Failed to allocate wallpaper PSRAM cache: %u bytes", static_cast<unsigned>(file_size));
        return false;
    }

    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        heap_caps_free(data);
        return false;
    }
    int64_t start_ms = esp_timer_get_time() / 1000;
    size_t read = fread(data, 1, file_size, file);
    fclose(file);
    if (read != file_size) {
        ESP_LOGW(TAG, "Failed to preload wallpaper: read=%u expected=%u", static_cast<unsigned>(read), static_cast<unsigned>(file_size));
        heap_caps_free(data);
        return false;
    }

    cached_wallpaper_ = data;
    cached_wallpaper_size_ = file_size;
    ESP_LOGI(TAG, "Preloaded wallpaper to PSRAM: %u bytes in %dms, largest_psram_after=%u",
        static_cast<unsigned>(file_size), static_cast<int>(esp_timer_get_time() / 1000 - start_ms),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    return true;
}
