#include "buzz_sdcard.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "file_index.h"
#include "rec_meta_esp.h"
#include "sdmmc_cmd.h"

static const char *TAG = "buzz_sdcard";
static bool s_mounted;
static esp_err_t s_last_error = ESP_ERR_INVALID_STATE;

#define ROOT_LIST_MAX 20

static esp_err_t mount_card(bool format_if_mount_failed)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    const sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    const sdmmc_slot_config_t slot_config = {
        .clk = BSP_SD_CLK,
        .cmd = BSP_SD_CMD,
        .d0 = BSP_SD_D0,
        .d1 = GPIO_NUM_NC,
        .d2 = GPIO_NUM_NC,
        .d3 = GPIO_NUM_NC,
        .d4 = GPIO_NUM_NC,
        .d5 = GPIO_NUM_NC,
        .d6 = GPIO_NUM_NC,
        .d7 = GPIO_NUM_NC,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = 0,
    };
    return esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot_config, &mount_config,
                                   &bsp_sdcard);
}

esp_err_t buzz_sdcard_probe(void)
{
    s_mounted = false;
    esp_err_t ret = mount_card(false);
    if (ret != ESP_OK) {
        s_last_error = ret;
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_mounted = true;
    s_last_error = ESP_OK;
    const sdmmc_card_t *card = bsp_sdcard;
    uint64_t cap_mb = (uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024);
    ESP_LOGI(TAG, "mounted at %s: name=%s capacity=%" PRIu64 "MB max_freq=%" PRIu32 "kHz",
             BSP_SD_MOUNT_POINT, card->cid.name, cap_mb, card->max_freq_khz);

    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGE(TAG, "opendir %s failed", BSP_SD_MOUNT_POINT);
        bsp_sdcard_unmount();
        s_mounted = false;
        s_last_error = ESP_FAIL;
        return ESP_FAIL;
    }
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(dir)) != NULL && count < ROOT_LIST_MAX) {
        ESP_LOGI(TAG, "  %s", ent->d_name);
        count++;
    }
    if (count == 0) {
        ESP_LOGI(TAG, "  (empty)");
    }
    closedir(dir);
    return ESP_OK;
}

uint32_t buzz_sdcard_cleanup_orphans(void)
{
    if (!s_mounted) {
        return 0;
    }
    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGW(TAG, "orphan scan: opendir failed");
        return 0;
    }
    uint32_t removed = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char full[280];
        snprintf(full, sizeof(full), "%s/%s", BSP_SD_MOUNT_POINT, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        if (!buzz_file_orphan_candidate(ent->d_name, (uint32_t)st.st_size,
                                        S_ISREG(st.st_mode))) {
            continue;
        }
        if (remove(full) == 0) {
            (void)rec_meta_remove(ent->d_name);
            removed++;
            ESP_LOGW(TAG, "removed empty recording: %s", ent->d_name);
        } else {
            ESP_LOGW(TAG, "remove empty recording failed: %s", ent->d_name);
        }
    }
    closedir(dir);
    return removed;
}

esp_err_t buzz_sdcard_format(void)
{
    if (s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "explicit format requested");
    esp_err_t ret = mount_card(true);
    s_last_error = ret;
    s_mounted = ret == ESP_OK;
    if (ret != ESP_OK) {
        return ret;
    }
    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (dir == NULL) {
        bsp_sdcard_unmount();
        s_mounted = false;
        s_last_error = ESP_FAIL;
        return ESP_FAIL;
    }
    closedir(dir);
    ESP_LOGI(TAG, "format and remount complete");
    return ESP_OK;
}

bool buzz_sdcard_mounted(void)
{
    return s_mounted;
}

esp_err_t buzz_sdcard_last_error(void)
{
    return s_last_error;
}
