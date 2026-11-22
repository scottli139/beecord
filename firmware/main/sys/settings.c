// Beecord 设置框架实现（settings.c，M5.4）。硬件无关，存储回调注入。
#include "settings.h"

#include "rtc.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

typedef enum {
    SETTING_T_U8 = 0,
    SETTING_T_FLOAT,
    SETTING_T_TIME_CONTEXT,
} setting_type_t;

typedef struct {
    setting_type_t type;
    uint8_t def_u8;
    float def_float;
} setting_desc_t;

// 默认值表（默认行为 = M5.4 之前现状，见头文件注）
static const setting_desc_t s_desc[SETTINGS_ID_COUNT] = {
    [SETTINGS_REC_QUALITY] = { SETTING_T_U8, 0u, 0.0f },
    [SETTINGS_MIC_GAIN_DB] = { SETTING_T_FLOAT, 0u, 33.0f },
    [SETTINGS_BEEP_ENABLED] = { SETTING_T_U8, 1u, 0.0f },
    [SETTINGS_BRIGHTNESS] = { SETTING_T_U8, 100u, 0.0f },
    [SETTINGS_SCREEN_OFF_S] = { SETTING_T_U8, 10u, 0.0f },
    [SETTINGS_TIME_CONTEXT] = { SETTING_T_TIME_CONTEXT, 0u, 0.0f },
};

typedef struct {
    uint8_t u8;
    float f;
    settings_time_context_t time;
} setting_value_t;

static setting_value_t s_values[SETTINGS_ID_COUNT];
static const settings_store_t *s_store;
static void *s_ctx;
static bool s_save_ok = true;

void settings_init(const settings_store_t *store, void *ctx)
{
    s_store = store;
    s_ctx = ctx;
    s_save_ok = true;
    settings_reset_to_defaults();
}

void settings_reset_to_defaults(void)
{
    for (int i = 0; i < (int)SETTINGS_ID_COUNT; i++) {
        s_values[i].u8 = s_desc[i].def_u8;
        s_values[i].f = s_desc[i].def_float;
        memset(&s_values[i].time, 0, sizeof(s_values[i].time));
    }
}

// 集中值域校验（见头文件约定）
static bool u8_value_valid(settings_id_t id, uint8_t v)
{
    switch (id) {
    case SETTINGS_REC_QUALITY:
    case SETTINGS_BEEP_ENABLED:
        return v <= 1u;
    case SETTINGS_BRIGHTNESS:
        return v <= 100u;
    case SETTINGS_SCREEN_OFF_S:
        return v == 0u || v == 10u || v == 30u || v == 60u; // ux-device 档位（0=永不）
    default:
        return false;
    }
}

static bool float_value_valid(settings_id_t id, float v)
{
    if (id == SETTINGS_MIC_GAIN_DB) {
        return isfinite(v) && v >= 0.0f && v <= 37.5f; // ES7210 PGA 量程
    }
    return false;
}

void settings_load_all(void)
{
    if (s_store == NULL) {
        return;
    }
    for (uint8_t id = 0; id < (uint8_t)SETTINGS_ID_COUNT; id++) {
        if (s_desc[id].type == SETTING_T_U8) {
            uint8_t v = 0;
            const int n = s_store->load(s_ctx, id, &v, sizeof(v));
            if (n == (int)sizeof(v) && u8_value_valid((settings_id_t)id, v)) {
                s_values[id].u8 = v;
            }
        } else if (s_desc[id].type == SETTING_T_FLOAT) {
            float v = 0.0f;
            const int n = s_store->load(s_ctx, id, &v, sizeof(v));
            if (n == (int)sizeof(v) && float_value_valid((settings_id_t)id, v)) {
                s_values[id].f = v;
            }
        } else {
            settings_time_context_t v;
            const int n = s_store->load(s_ctx, id, &v, sizeof(v));
            if (n == (int)sizeof(v) && v.synced <= 1u && v.reserved == 0u &&
                v.utc_offset_min >= RTC_OFFSET_MIN_M && v.utc_offset_min <= RTC_OFFSET_MAX_M) {
                s_values[id].time = v;
            }
        }
    }
}

uint8_t settings_get_u8(settings_id_t id)
{
    if ((int)id < 0 || id >= SETTINGS_ID_COUNT || s_desc[id].type != SETTING_T_U8) {
        return 0;
    }
    return s_values[id].u8;
}

float settings_get_float(settings_id_t id)
{
    if ((int)id < 0 || id >= SETTINGS_ID_COUNT || s_desc[id].type != SETTING_T_FLOAT) {
        return 0.0f;
    }
    return s_values[id].f;
}

static void persist(settings_id_t id)
{
    s_save_ok = true;
    if (s_store == NULL || s_store->save == NULL) {
        return; // 纯运行态：视为成功（无持久化义务）
    }
    int rc;
    if (s_desc[id].type == SETTING_T_U8) {
        rc = s_store->save(s_ctx, (uint8_t)id, &s_values[id].u8, sizeof(uint8_t));
    } else if (s_desc[id].type == SETTING_T_FLOAT) {
        rc = s_store->save(s_ctx, (uint8_t)id, &s_values[id].f, sizeof(float));
    } else {
        rc = s_store->save(s_ctx, (uint8_t)id, &s_values[id].time,
                           sizeof(settings_time_context_t));
    }
    if (rc < 0) {
        s_save_ok = false; // 运行值保留；上层观测 settings_last_save_ok
    }
}

void settings_set_u8(settings_id_t id, uint8_t value)
{
    if ((int)id < 0 || id >= SETTINGS_ID_COUNT || s_desc[id].type != SETTING_T_U8) {
        return;
    }
    if (!u8_value_valid(id, value)) {
        return; // 非法值：拒绝并保留原值（load/set 同规则）
    }
    s_values[id].u8 = value;
    persist(id);
}

void settings_set_float(settings_id_t id, float value)
{
    if ((int)id < 0 || id >= SETTINGS_ID_COUNT || s_desc[id].type != SETTING_T_FLOAT) {
        return;
    }
    if (!float_value_valid(id, value)) {
        return; // NaN/Inf/越域：拒绝并保留原值
    }
    s_values[id].f = value;
    persist(id);
}

settings_time_context_t settings_get_time_context(void)
{
    return s_values[SETTINGS_TIME_CONTEXT].time;
}

void settings_set_time_context(int utc_offset_min)
{
    if (utc_offset_min < RTC_OFFSET_MIN_M || utc_offset_min > RTC_OFFSET_MAX_M) {
        return;
    }
    settings_time_context_t value = {
        .utc_offset_min = (int16_t)utc_offset_min,
        .synced = 1u,
        .reserved = 0u,
    };
    s_values[SETTINGS_TIME_CONTEXT].time = value;
    persist(SETTINGS_TIME_CONTEXT);
}

bool settings_last_save_ok(void)
{
    return s_save_ok;
}
