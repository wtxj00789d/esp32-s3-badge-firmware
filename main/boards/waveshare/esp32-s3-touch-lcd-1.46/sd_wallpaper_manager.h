#ifndef SD_WALLPAPER_MANAGER_H_
#define SD_WALLPAPER_MANAGER_H_

#include "gifp_player.h"

#include <string>
#include <vector>

#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include <sdmmc_cmd.h>
#include "esp_io_expander_tca9554.h"

class SdWallpaperManager {
public:
    SdWallpaperManager(esp_io_expander_handle_t io_expander, esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io);
    ~SdWallpaperManager();

    bool Initialize();
    bool SwitchToNext();
    void ShowDefaultPage();
    bool RestoreSavedWallpaper();
    bool wallpaper_mode() const { return wallpaper_mode_; }

private:
    esp_io_expander_handle_t io_expander_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    sdmmc_card_t* card_ = nullptr;
    bool mounted_ = false;
    bool wallpaper_mode_ = false;
    uint8_t* cached_wallpaper_ = nullptr;
    size_t cached_wallpaper_size_ = 0;
    std::vector<std::string> wallpapers_;
    int current_index_ = -1;
    GifpPlayer player_;

    bool Mount();
    void Unmount();
    void ScanWallpapers();
    void ScanWallpaperDirectory(const char* dir);
    void SaveCurrentPath();
    bool PlayIndex(int index);
    void FreeCachedWallpaper();
    bool LoadWallpaperToPsram(const std::string& path);
};

#endif
