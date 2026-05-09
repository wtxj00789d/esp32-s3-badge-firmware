#include "badge_settings.h"

#include <esp_log.h>

#include "settings.h"

namespace {
constexpr const char* TAG = "BadgeSettings";
constexpr const char* kNamespace = "badge";
constexpr const char* kCurrentKey = "current";
constexpr const char* kRecordingNextKey = "rec_next";
}

bool BadgeSettings::Open()
{
    Settings settings(kNamespace, true);
    opened_ = settings.IsOpen();
    if (opened_) {
        ESP_LOGI(TAG, "Opened badge settings namespace");
    } else {
        ESP_LOGW(TAG, "Badge settings namespace is unavailable");
    }
    return opened_;
}

std::string BadgeSettings::CurrentBasename() const
{
    Settings settings(kNamespace, false);
    return settings.GetString(kCurrentKey);
}

void BadgeSettings::SetCurrentBasename(const std::string& basename)
{
    Settings settings(kNamespace, true);
    settings.SetString(kCurrentKey, basename);
}

int BadgeSettings::NextRecordingNumber()
{
    Settings settings(kNamespace, true);
    int number = settings.GetInt(kRecordingNextKey, 1);
    if (number < 1) {
        number = 1;
    }
    settings.SetInt(kRecordingNextKey, number + 1);
    return number;
}
