#include "buzz_sdcard.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdint.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"

static const char *TAG = "buzz_sdcard";

#define ROOT_LIST_MAX 20

esp_err_t buzz_sdcard_probe(void)
{
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const sdmmc_card_t *card = bsp_sdcard;
    uint64_t cap_mb = (uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024);
    ESP_LOGI(TAG, "mounted at %s: name=%s capacity=%" PRIu64 "MB max_freq=%" PRIu32 "kHz",
             BSP_SD_MOUNT_POINT, card->cid.name, cap_mb, card->max_freq_khz);

    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGE(TAG, "opendir %s failed", BSP_SD_MOUNT_POINT);
        bsp_sdcard_unmount();
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
