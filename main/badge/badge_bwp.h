#pragma once

#include <cstddef>
#include <cstdint>

class BadgeBwp {
public:
    BadgeBwp() = default;
    ~BadgeBwp();

    BadgeBwp(const BadgeBwp&) = delete;
    BadgeBwp& operator=(const BadgeBwp&) = delete;

    bool Load(const char* path);
    void Close();
    bool loaded() const { return buffer_ != nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }
    int fps() const { return fps_; }
    int frame_count() const { return frame_count_; }
    const uint16_t* Frame(int index) const;

private:
    uint8_t* buffer_ = nullptr;
    size_t buffer_size_ = 0;
    const uint16_t* frames_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 0;
    int frame_count_ = 0;
};
