#ifndef GIFP_PLAYER_H_
#define GIFP_PLAYER_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <lvgl.h>

class GifpPlayer {
public:
    GifpPlayer();
    ~GifpPlayer();

    bool StartFile(const std::string& path);
    bool StartMemory(const uint8_t* data, size_t size);
    void Stop();
    void SetPanelHandle(esp_lcd_panel_handle_t panel) { panel_ = panel; }
    void SetPanelIoHandle(esp_lcd_panel_io_handle_t panel_io) { panel_io_ = panel_io; }
    bool running() const { return task_ != nullptr; }

private:
    enum class SourceType { None, File, Memory };

    struct Source {
        SourceType type = SourceType::None;
        std::string path;
        FILE* file = nullptr;
        const uint8_t* memory = nullptr;
        size_t size = 0;

        bool Open();
        void Close();
        bool Read(uint32_t offset, void* dst, uint32_t len);
    };

    Source source_;
    TaskHandle_t task_ = nullptr;
    volatile bool stop_requested_ = false;

    lv_obj_t* layer_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_image_dsc_t image_dsc_{};
    uint8_t* frame_buffer_ = nullptr;
    uint8_t* display_buffer_ = nullptr;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    uint32_t frame_bytes_ = 0;
    uint32_t display_buffer_bytes_ = 0;
    uint32_t object_generation_ = 0;
    bool transparent_color_key_ = false;
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    uint16_t dirty_x_ = 0;
    uint16_t dirty_y_ = 0;
    uint16_t dirty_w_ = 0;
    uint16_t dirty_h_ = 0;

    static void TaskEntry(void* arg);
    static void DeleteLvglObjectsAsync(void* arg);
    bool StopInternal(bool delete_lvgl_objects);
    void Run();
    bool DecodeAndPlay();
    void DrawFrame();
    bool WaitPanelTransferDone();
    bool DrawIndexedRawFrame(const uint16_t* palette, uint16_t palette_count, const uint8_t* indices, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    bool UseDirectPanelDraw() const;
    uint8_t DirectPanelScale() const;
    void SetDirtyFullFrame();
    void CreateOrUpdateLvglObjects();
    void DeleteLvglObjects();
};

#endif
