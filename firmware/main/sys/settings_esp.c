// Beecord 设置框架 ESP NVS 适配实现（settings_esp.c，M5.4）。
#include "settings_esp.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
#define NVS_NS "bee_cfg"

static nvs_handle_t s_h;

// 键表（NVS key 长度 ≤15，用短名）
static const char *s_keys[SETTINGS_ID_COUNT] = {
    [SETTINGS_REC_QUALITY] = "recq",
    [SETTINGS_MIC_GAIN_DB] = "micg",
    [SETTINGS_BEEP_ENABLED] = "beep",
    [SETTINGS_BRIGHTNESS] = "brig",
    [SETTINGS_SCREEN_OFF_S] = "scro",
};

// settings_id → 是否 float 项（与 settings.c 的类型表保持一致）
static const bool s_is_float[SETTINGS_ID_COUNT] = {
    [SETTINGS_REC_QUALITY] = false,
    [SETTINGS_MIC_GAIN_DB] = true,
    [SETTINGS_BEEP_ENABLED] = false,
    [SETTINGS_BRIGHTNESS] = false,
    [SETTINGS_SCREEN_OFF_S] = false,
};

static int nvs_load(void *ctx, uint8_t key, void *buf, uint32_t cap)
{
    (void)ctx;
    if (key >= (uint8_t)SETTINGS_ID_COUNT || s_h == 0) {
        return -1;
    }
    if (s_is_float[key]) {
        // ESP-IDF NVS 无 float 原语：按 IEEE754 位模式存 u32
        uint32_t bits = 0;
        if (nvs_get_u32(s_h, s_keys[key], &bits) != ESP_OK) {
            return -1;
        }
        if (cap < sizeof(float)) {
            return -1;
        }
        memcpy(buf, &bits, sizeof(float));
        return (int)sizeof(float);
    }
    uint8_t v = 0;
    if (nvs_get_u8(s_h, s_keys[key], &v) != ESP_OK) {
        return -1;
    }
    if (cap < sizeof(v)) {
        return -1;
    }
    memcpy(buf, &v, sizeof(v));
    return (int)sizeof(v);
}

static int nvs_save(void *ctx, uint8_t key, const void *buf, uint32_t len)
{
    (void)ctx;
    if (key >= (uint8_t)SETTINGS_ID_COUNT || s_h == 0) {
        return -1;
    }
    esp_err_t ret;
    if (s_is_float[key]) {
        if (len != sizeof(float)) {
            return -1;
        }
        uint32_t bits = 0;
        memcpy(&bits, buf, sizeof(bits));
        ret = nvs_set_u32(s_h, s_keys[key], bits);
    } else {
        if (len != sizeof(uint8_t)) {
            return -1;
        }
        ret = nvs_set_u8(s_h, s_keys[key], *(const uint8_t *)buf);
    }
    if (ret != ESP_OK) {
        return -1;
    }
    return nvs_commit(s_h) == ESP_OK ? 0 : -1;
}

esp_err_t settings_esp_init(void)
{
    if (s_h != 0) {
        return ESP_OK; // 幂等
    }
    static const settings_store_t store = { .load = nvs_load, .save = nvs_save };
    const esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &s_h);
    if (ret != ESP_OK) {
        // 失败路径必须显式初始化默认值——否则静态零值被当成设置应用
        //（M5.4 复查实锤：gain=0/beep=off/timeout=0）。回退纯运行态默认值。
        ESP_LOGE(TAG, "nvs open %s failed (%s), defaults in use", NVS_NS,
                 esp_err_to_name(ret));
        settings_init(NULL, NULL);
        return ret;
    }
    settings_init(&store, NULL);
    settings_load_all();
    ESP_LOGI(TAG, "settings loaded (nvs ns=%s)", NVS_NS);
    return ESP_OK;
}

void settings_esp_dump(void)
{
    ESP_LOGI(TAG, "rec_quality=%u mic_gain=%.1fdB beep=%u brightness=%u screen_off=%us",
             (unsigned)settings_get_u8(SETTINGS_REC_QUALITY),
             (double)settings_get_float(SETTINGS_MIC_GAIN_DB),
             (unsigned)settings_get_u8(SETTINGS_BEEP_ENABLED),
             (unsigned)settings_get_u8(SETTINGS_BRIGHTNESS),
             (unsigned)settings_get_u8(SETTINGS_SCREEN_OFF_S));
}
