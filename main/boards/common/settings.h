#pragma once

#include <cstdint>
#include <string>

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false)
        : ns_(ns), read_write_(read_write)
    {
    }

    int32_t GetInt(const std::string& key, int32_t default_value = 0)
    {
        (void)key;
        return default_value;
    }

    void SetInt(const std::string& key, int32_t value)
    {
        (void)key;
        (void)value;
    }

private:
    std::string ns_;
    bool read_write_;
};
