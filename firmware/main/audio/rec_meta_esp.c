#include "rec_meta_esp.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"

static int meta_path(const char *audio_name, char *out, size_t out_cap)
{
    char sidecar[REC_META_NAME_MAX + 1u];
    if (rec_meta_sidecar_name(audio_name, sidecar, sizeof(sidecar)) != 0) {
        return -1;
    }
    const int n = snprintf(out, out_cap, "%s/%s", BSP_SD_MOUNT_POINT, sidecar);
    return n > 0 && (size_t)n < out_cap ? 0 : -1;
}

esp_err_t rec_meta_save(const char *audio_name, const rec_meta_t *meta)
{
    char path[sizeof(BSP_SD_MOUNT_POINT) + REC_META_NAME_MAX + 2u];
    char json[REC_META_JSON_CAP];
    if (meta_path(audio_name, path, sizeof(path)) != 0 ||
        rec_meta_encode(meta, json, sizeof(json)) < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return ESP_FAIL;
    }
    const size_t len = strlen(json);
    bool ok = fwrite(json, 1, len, fp) == len && fflush(fp) == 0;
    if (ok && fsync(fileno(fp)) != 0) {
        ok = false;
    }
    if (fclose(fp) != 0) {
        ok = false;
    }
    if (!ok) {
        (void)unlink(path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t rec_meta_remove(const char *audio_name)
{
    char path[sizeof(BSP_SD_MOUNT_POINT) + REC_META_NAME_MAX + 2u];
    if (meta_path(audio_name, path, sizeof(path)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
