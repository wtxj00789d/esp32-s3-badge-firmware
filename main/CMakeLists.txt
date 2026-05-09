set(SOURCES
    "main.cc"
    "badge/badge_application.cc"
    "badge/badge_board.cc"
    "badge/badge_defaults.cc"
    "badge/badge_input.cc"
    "badge/badge_storage.cc"
    "badge/badge_settings.cc"
    "badge/badge_bwp.cc"
    "badge/badge_sound.cc"
    "badge/badge_recorder.cc"
    "audio/audio_codec.cc"
    "audio/codecs/es8311_audio_codec.cc"
    "boards/common/backlight.cc"
    "boards/common/button.cc"
    "boards/common/i2c_device.cc"
    "boards/common/system_reset.cc"
    "led/single_led.cc"
    "settings.cc"
)

set(INCLUDE_DIRS
    "."
    "badge"
    "audio"
    "audio/codecs"
    "boards/common"
    "led"
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES nvs_flash fatfs sdmmc esp_driver_sdmmc
)
