#include "badge_application.h"

#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

    std::vector<uint16_t> frame(board_.Width() * board_.Height(), 0x001F);
    board_.DrawRgb565(0, 0, board_.Width(), board_.Height(), frame.data());
}

void BadgeApplication::Run()
{
    ESP_LOGI(TAG, "Run badge firmware");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
