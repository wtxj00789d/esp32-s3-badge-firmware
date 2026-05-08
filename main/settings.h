#pragma once

#include <cstdint>
#include <string>

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false)
        : ns_(ns), read_write_(read_write)
    {
    }

    ~Settings() = default;

    std::string GetString(const std::string& key, const std::string& default_value = "")
    {
        (void)key;
        return default_value;
    }

    void SetString(const std::string& key, const std::string& value)
    {
        (void)key;
        (void)value;
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

    bool GetBool(const std::string& key, bool default_value = false)
    {
        (void)key;
        return default_value;
    }

    void SetBool(const std::string& key, bool value)
    {
        (void)key;
        (void)value;
    }

    void EraseKey(const std::string& key)
    {
        (void)key;
    }

    void EraseAll() {}

private:
    std::string ns_;
    bool read_write_;
};
