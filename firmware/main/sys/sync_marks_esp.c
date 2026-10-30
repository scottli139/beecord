// Beecord 已同步标记 ESP NVS 适配实现（sync_marks_esp.c，M5.4）。
#include "sync_marks_esp.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "syncmarks";
#define NVS_NS "bee_sync"
#define NVS_KEY "marks"

static nvs_handle_t s_h;

static int blob_load(void *ctx, uint8_t *buf, uint32_t cap)
{
    (void)ctx;
    if (s_h == 0) {
        return -1;
    }
    size_t len = cap;
    if (nvs_get_blob(s_h, NVS_KEY, buf, &len) != ESP_OK) {
        return -1;
    }
    return (int)len;
}

static int blob_save(void *ctx, const uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (s_h == 0) {
        return -1;
    }
    if (nvs_set_blob(s_h, NVS_KEY, buf, len) != ESP_OK) {
        return -1;
    }
    return nvs_commit(s_h) == ESP_OK ? 0 : -1;
}

esp_err_t sync_marks_esp_init(void)
{
    if (s_h != 0) {
        return ESP_OK; // 幂等
    }
    const esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &s_h);
    if (ret != ESP_OK) {
        // 失败路径显式初始化为纯运行态（不依赖静态零初始化，同 settings 修复）
        ESP_LOGE(TAG, "nvs open %s failed (%s), runtime-only", NVS_NS,
                 esp_err_to_name(ret));
        sync_marks_init(NULL, NULL);
        return ret;
    }
    static const sync_marks_store_t store = { .load = blob_load, .save = blob_save };
    sync_marks_init(&store, NULL);
    ESP_LOGI(TAG, "sync marks loaded: %u/%u", (unsigned)sync_marks_count(),
             (unsigned)sync_marks_capacity());
    return ESP_OK;
}
