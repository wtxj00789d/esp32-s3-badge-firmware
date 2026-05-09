#include "badge_application.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "badge_defaults.h"

namespace {
constexpr const char* TAG = "BadgeApplication";

const char* ButtonEventName(BadgeButtonEvent event)
{
    switch (event) {
    case BadgeButtonEvent::SingleClick:
        return "single click";
    case BadgeButtonEvent::DoubleClick:
        return "double click";
    case BadgeButtonEvent::LongPress:
        return "long press";
    }
    return "unknown";
}
}

void BadgeApplication::Initialize()
{
    ESP_LOGI(TAG, "Initialize badge firmware");
    board_.Initialize();
    board_.SetButtonCallback([](BadgeButtonEvent event) {
        ESP_LOGI(TAG, "BOOT button: %s", ButtonEventName(event));
    });

    board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), badge_defaults::DefaultPage());
}

void BadgeApplication::Run()
{
    ESP_LOGI(TAG, "Run badge firmware");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
