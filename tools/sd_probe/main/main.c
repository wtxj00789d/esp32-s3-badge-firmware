#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_probe";

#define SD_CLK_GPIO GPIO_NUM_17
#define SD_CMD_GPIO GPIO_NUM_18
#define SD_D0_GPIO GPIO_NUM_21
#define SD_D3_GPIO GPIO_NUM_13

static void print_fat_info(void) {
    uint64_t total = 0;
    uint64_t free = 0;
    esp_err_t ret = esp_vfs_fat_info("/sdcard", &total, &free);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "fat info: total=%llu free=%llu bytes",
                 (unsigned long long)total, (unsigned long long)free);
    } else {
        ESP_LOGW(TAG, "esp_vfs_fat_info failed: %s (0x%x)", esp_err_to_name(ret), ret);
    }
}

static void list_root(void) {
    DIR *dir = opendir("/sdcard");
    if (dir == NULL) {
        ESP_LOGW(TAG, "opendir /sdcard failed: errno=%d (%s)", errno, strerror(errno));
        return;
    }
    ESP_LOGI(TAG, "root directory:");
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, "  %s", entry->d_name);
    }
    closedir(dir);
}

static esp_err_t try_write_file(const char *path, const char *mode) {
    errno = 0;
    FILE *f = fopen(path, mode);
    if (f == NULL) {
        ESP_LOGE(TAG, "open %s mode=%s failed: errno=%d (%s)", path, mode, errno, strerror(errno));
        return ESP_FAIL;
    }
    fprintf(f, "sd_probe ok\n");
    fprintf(f, "clk=%d cmd=%d d0=%d d3=%d\n", SD_CLK_GPIO, SD_CMD_GPIO, SD_D0_GPIO, SD_D3_GPIO);
    fclose(f);

    f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "readback %s failed: errno=%d (%s)", path, errno, strerror(errno));
        return ESP_FAIL;
    }
    char line[64] = {0};
    fgets(line, sizeof(line), f);
    fclose(f);
    ESP_LOGI(TAG, "readback first line: %s", line);
    return ESP_OK;
}

static esp_err_t write_probe_file(void) {
    if (try_write_file("/sdcard/PROBE.TXT", "w") == ESP_OK) {
        return ESP_OK;
    }
    if (try_write_file("/sdcard/probe.txt", "w") == ESP_OK) {
        return ESP_OK;
    }
    mkdir("/sdcard/BADGE", 0775);
    return try_write_file("/sdcard/BADGE/PROBE.TXT", "w");
}

static esp_err_t write_speed_probe(void) {
    static uint8_t buffer[4096];
    for (size_t i = 0; i < sizeof(buffer); ++i) {
        buffer[i] = (uint8_t)(i & 0xff);
    }

    FILE *f = fopen("/sdcard/WRITE.BIN", "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open WRITE.BIN failed: errno=%d (%s)", errno, strerror(errno));
        return ESP_FAIL;
    }

    const size_t total = 1024 * 1024;
    int64_t start_us = esp_timer_get_time();
    size_t written = 0;
    while (written < total) {
        size_t chunk = total - written;
        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }
        if (fwrite(buffer, 1, chunk, f) != chunk) {
            ESP_LOGE(TAG, "fwrite failed at %u bytes: errno=%d (%s)", (unsigned)written, errno, strerror(errno));
            fclose(f);
            return ESP_FAIL;
        }
        written += chunk;
    }
    fflush(f);
    fclose(f);
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    double kb_s = (double)written * 1000000.0 / (double)elapsed_us / 1024.0;
    ESP_LOGI(TAG, "write speed: %u bytes in %lld us = %.1f KiB/s",
             (unsigned)written, (long long)elapsed_us, kb_s);
    return ESP_OK;
}

static esp_err_t recording_write_probe(void) {
    static uint8_t buffer[1024];
    for (size_t i = 0; i < sizeof(buffer); ++i) {
        buffer[i] = (uint8_t)((i * 31) & 0xff);
    }

    FILE *f = fopen("/sdcard/REC_TEST.BIN", "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open REC_TEST.BIN failed: errno=%d (%s)", errno, strerror(errno));
        return ESP_FAIL;
    }

    const uint32_t duration_ms = 60000;
    const uint32_t bytes_per_second = 48000;
    const uint32_t chunk_bytes = sizeof(buffer);
    const uint32_t total_bytes = bytes_per_second * (duration_ms / 1000);
    uint32_t written = 0;
    int64_t start_us = esp_timer_get_time();
    int64_t next_write_us = start_us;
    ESP_LOGI(TAG, "recording write probe start: target=%u bytes over %u ms",
             (unsigned)total_bytes, (unsigned)duration_ms);

    while (written < total_bytes) {
        int64_t now_us = esp_timer_get_time();
        if (now_us < next_write_us) {
            vTaskDelay(pdMS_TO_TICKS((next_write_us - now_us + 999) / 1000));
        }

        size_t chunk = total_bytes - written;
        if (chunk > chunk_bytes) {
            chunk = chunk_bytes;
        }
        if (fwrite(buffer, 1, chunk, f) != chunk) {
            ESP_LOGE(TAG, "recording fwrite failed at %u bytes: errno=%d (%s)",
                     (unsigned)written, errno, strerror(errno));
            fclose(f);
            return ESP_FAIL;
        }
        written += chunk;
        next_write_us = start_us + ((int64_t)written * 1000000) / bytes_per_second;
        if ((written % (bytes_per_second * 10)) < chunk_bytes) {
            ESP_LOGI(TAG, "recording progress: %u/%u bytes", (unsigned)written, (unsigned)total_bytes);
        }
    }

    int64_t flush_start_us = esp_timer_get_time();
    fflush(f);
    int64_t flush_us = esp_timer_get_time() - flush_start_us;
    fclose(f);
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    ESP_LOGI(TAG, "recording write probe done: written=%u elapsed=%lld us flush=%lld us",
             (unsigned)written, (long long)elapsed_us, (long long)flush_us);

    struct stat st = {0};
    if (stat("/sdcard/REC_TEST.BIN", &st) == 0) {
        ESP_LOGI(TAG, "REC_TEST.BIN size=%lld", (long long)st.st_size);
        return st.st_size == total_bytes ? ESP_OK : ESP_FAIL;
    }
    ESP_LOGE(TAG, "stat REC_TEST.BIN failed: errno=%d (%s)", errno, strerror(errno));
    return ESP_FAIL;
}

void app_main(void) {
    ESP_LOGI(TAG, "Spotpear ESP32-S3 1.28 SD probe starting");
    ESP_LOGI(TAG, "pins: CLK=%d CMD=%d D0=%d D3/CD=%d", SD_CLK_GPIO, SD_CMD_GPIO, SD_D0_GPIO, SD_D3_GPIO);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SD_CLK_GPIO;
    slot_config.cmd = SD_CMD_GPIO;
    slot_config.d0 = SD_D0_GPIO;
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = SD_D3_GPIO;
    slot_config.d4 = GPIO_NUM_NC;
    slot_config.d5 = GPIO_NUM_NC;
    slot_config.d6 = GPIO_NUM_NC;
    slot_config.d7 = GPIO_NUM_NC;
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (0x%x)", esp_err_to_name(ret), ret);
        return;
    }

    ESP_LOGI(TAG, "mount ok");
    sdmmc_card_print_info(stdout, card);
    print_fat_info();
    list_root();

    struct stat st = {0};
    if (stat("/sdcard", &st) == 0) {
        ESP_LOGI(TAG, "/sdcard stat ok");
    }

    write_probe_file();
    write_speed_probe();
    recording_write_probe();

    esp_vfs_fat_sdcard_unmount("/sdcard", card);
    ESP_LOGI(TAG, "probe complete");
}
