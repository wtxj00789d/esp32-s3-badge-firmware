#pragma once

#include <string>

class BadgeSettings {
public:
    bool Open();
    std::string CurrentBasename() const;
    void SetCurrentBasename(const std::string& basename);
    int NextRecordingNumber();

private:
    bool opened_ = false;
};
