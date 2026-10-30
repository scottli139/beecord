// settings 设置框架宿主单测（M5.4）：默认值、存取往返、注入后端持久化、
// 写失败语义、类型表完整性。内存后端模拟 NVS。
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <math.h>

#include "../main/sys/settings.h"
#include "test.h"

// 内存后端：key→定长槽（u8=1B / float=4B），带"写失败开关"与加载计数
static uint8_t s_mem[SETTINGS_ID_COUNT][8];
static int s_mem_len[SETTINGS_ID_COUNT];
static bool s_fail_save;
static int s_load_calls;

static int mem_load(void *ctx, uint8_t key, void *buf, uint32_t cap)
{
    (void)ctx;
    s_load_calls++;
    if (key >= (uint8_t)SETTINGS_ID_COUNT || s_mem_len[key] < 0) {
        return -1;
    }
    if (cap < (uint32_t)s_mem_len[key]) {
        return -1;
    }
    memcpy(buf, s_mem[key], (size_t)s_mem_len[key]);
    return s_mem_len[key];
}

static int mem_save(void *ctx, uint8_t key, const void *buf, uint32_t len)
{
    (void)ctx;
    if (s_fail_save) {
        return -1;
    }
    if (key >= (uint8_t)SETTINGS_ID_COUNT || len > sizeof(s_mem[0])) {
        return -1;
    }
    memcpy(s_mem[key], buf, len);
    s_mem_len[key] = (int)len;
    return 0;
}

static void backend_reset(void)
{
    memset(s_mem, 0, sizeof(s_mem));
    memset(s_mem_len, 0xFF, sizeof(s_mem_len)); // 全 -1：无键
    s_fail_save = false;
    s_load_calls = 0;
}

static const settings_store_t s_store = { .load = mem_load, .save = mem_save };

static void test_defaults_match_current_behavior(void)
{
    backend_reset();
    settings_init(&s_store, NULL);
    CHECK(settings_get_u8(SETTINGS_REC_QUALITY) == 0u);
    CHECK(settings_get_float(SETTINGS_MIC_GAIN_DB) == 33.0f);
    CHECK(settings_get_u8(SETTINGS_BEEP_ENABLED) == 1u);
    CHECK(settings_get_u8(SETTINGS_BRIGHTNESS) == 100u);
    CHECK(settings_get_u8(SETTINGS_SCREEN_OFF_S) == 10u);
}

static void test_set_get_roundtrip(void)
{
    backend_reset();
    settings_init(&s_store, NULL);
    settings_set_u8(SETTINGS_REC_QUALITY, 1u);
    settings_set_float(SETTINGS_MIC_GAIN_DB, 25.5f);
    settings_set_u8(SETTINGS_BEEP_ENABLED, 0u);
    CHECK(settings_get_u8(SETTINGS_REC_QUALITY) == 1u);
    CHECK(settings_get_float(SETTINGS_MIC_GAIN_DB) == 25.5f);
    CHECK(settings_get_u8(SETTINGS_BEEP_ENABLED) == 0u);
}

static void test_persist_and_reload(void)
{
    backend_reset();
    settings_init(&s_store, NULL);
    settings_set_u8(SETTINGS_REC_QUALITY, 1u);
    settings_set_u8(SETTINGS_SCREEN_OFF_S, 60u);
    // 模拟重启：重新 init + load（同一块后端内存）
    settings_init(&s_store, NULL);
    CHECK(settings_get_u8(SETTINGS_REC_QUALITY) == 0u); // 载入前是默认
    settings_load_all();
    CHECK(settings_get_u8(SETTINGS_REC_QUALITY) == 1u);
    CHECK(settings_get_u8(SETTINGS_SCREEN_OFF_S) == 60u);
    CHECK(settings_get_float(SETTINGS_MIC_GAIN_DB) == 33.0f); // 未写过→默认
}

static void test_load_missing_key_keeps_default(void)
{
    backend_reset();
    s_mem_len[SETTINGS_REC_QUALITY] = 1; // 只给一个键
    s_mem[SETTINGS_REC_QUALITY][0] = 1u;
    settings_init(&s_store, NULL);
    settings_load_all();
    CHECK(settings_get_u8(SETTINGS_REC_QUALITY) == 1u);
    CHECK(settings_get_u8(SETTINGS_BEEP_ENABLED) == 1u); // 其余默认
}

static void test_load_short_value_rejected(void)
{
    backend_reset();
    s_mem_len[SETTINGS_MIC_GAIN_DB] = 2; // float 项只存了 2 字节：坏
    settings_init(&s_store, NULL);
    settings_load_all();
    CHECK(settings_get_float(SETTINGS_MIC_GAIN_DB) == 33.0f);
}

static void test_save_failure_keeps_runtime(void)
{
    backend_reset();
    settings_init(&s_store, NULL);
    settings_set_u8(SETTINGS_REC_QUALITY, 1u); // 先成功持久化
    s_fail_save = true;
    settings_set_u8(SETTINGS_REC_QUALITY, 0u);
    CHECK(settings_get_u8(SETTINGS_REC_QUALITY) == 0u); // 运行值保留
    CHECK(!settings_last_save_ok());
    // 模拟重启：持久态仍是上次成功的 1
    settings_init(&s_store, NULL);
    settings_load_all();
    CHECK(settings_get_u8(SETTINGS_REC_QUALITY) == 1u);
}

static void test_null_store_runtime_only(void)
{
    settings_init(NULL, NULL);
    settings_set_u8(SETTINGS_BEEP_ENABLED, 0u);
    CHECK(settings_get_u8(SETTINGS_BEEP_ENABLED) == 0u);
    CHECK(settings_last_save_ok()); // 无持久化义务=成功
}

static void test_invalid_id_guarded(void)
{
    backend_reset();
    settings_init(&s_store, NULL);
    settings_set_u8((settings_id_t)99, 5u); // 不崩、无效果
    CHECK(settings_get_u8((settings_id_t)99) == 0u);
    settings_set_u8(SETTINGS_MIC_GAIN_DB, 5u); // 类型错配静默拒绝
    CHECK(settings_get_float(SETTINGS_MIC_GAIN_DB) == 33.0f);
}

static void test_set_u8_out_of_range_rejected(void)
{
    backend_reset();
    settings_init(&s_store, NULL);
    settings_set_u8(SETTINGS_REC_QUALITY, 5u); // 仅 0/1
    CHECK(settings_get_u8(SETTINGS_REC_QUALITY) == 0u);
    settings_set_u8(SETTINGS_BEEP_ENABLED, 2u);
    CHECK(settings_get_u8(SETTINGS_BEEP_ENABLED) == 1u);
    settings_set_u8(SETTINGS_BRIGHTNESS, 101u);
    CHECK(settings_get_u8(SETTINGS_BRIGHTNESS) == 100u);
    settings_set_u8(SETTINGS_BRIGHTNESS, 100u); // 边界合法
    CHECK(settings_get_u8(SETTINGS_BRIGHTNESS) == 100u);
    settings_set_u8(SETTINGS_SCREEN_OFF_S, 45u); // 非离散档
    CHECK(settings_get_u8(SETTINGS_SCREEN_OFF_S) == 10u);
    settings_set_u8(SETTINGS_SCREEN_OFF_S, 30u); // 档位合法
    CHECK(settings_get_u8(SETTINGS_SCREEN_OFF_S) == 30u);
}

static void test_set_gain_range_and_finite(void)
{
    backend_reset();
    settings_init(&s_store, NULL);
    const float nan_v = sqrtf(-1.0f);
    const float inf_v = HUGE_VALF;
    settings_set_float(SETTINGS_MIC_GAIN_DB, nan_v);
    CHECK(settings_get_float(SETTINGS_MIC_GAIN_DB) == 33.0f); // NaN 拒绝
    settings_set_float(SETTINGS_MIC_GAIN_DB, inf_v);
    CHECK(settings_get_float(SETTINGS_MIC_GAIN_DB) == 33.0f); // Inf 拒绝
    settings_set_float(SETTINGS_MIC_GAIN_DB, -1.0f);
    CHECK(settings_get_float(SETTINGS_MIC_GAIN_DB) == 33.0f);
    settings_set_float(SETTINGS_MIC_GAIN_DB, 40.0f);
    CHECK(settings_get_float(SETTINGS_MIC_GAIN_DB) == 33.0f);
    settings_set_float(SETTINGS_MIC_GAIN_DB, 25.5f); // REPL 合法浮点
    CHECK(settings_get_float(SETTINGS_MIC_GAIN_DB) == 25.5f);
    settings_set_float(SETTINGS_MIC_GAIN_DB, 0.0f); // 边界合法
    CHECK(settings_get_float(SETTINGS_MIC_GAIN_DB) == 0.0f);
    settings_set_float(SETTINGS_MIC_GAIN_DB, 37.5f); // 上限边界
    CHECK(settings_get_float(SETTINGS_MIC_GAIN_DB) == 37.5f);
}

static void test_load_garbage_values_rejected(void)
{
    backend_reset();
    // 模拟损坏 NVS：越域质量、超亮度、NaN 增益、离散档外熄屏
    s_mem_len[SETTINGS_REC_QUALITY] = 1;
    s_mem[SETTINGS_REC_QUALITY][0] = 9u;
    s_mem_len[SETTINGS_BRIGHTNESS] = 1;
    s_mem[SETTINGS_BRIGHTNESS][0] = 200u;
    s_mem_len[SETTINGS_SCREEN_OFF_S] = 1;
    s_mem[SETTINGS_SCREEN_OFF_S][0] = 99u;
    s_mem_len[SETTINGS_MIC_GAIN_DB] = 4;
    const float nan_v = sqrtf(-1.0f);
    memcpy(s_mem[SETTINGS_MIC_GAIN_DB], &nan_v, 4);
    settings_init(&s_store, NULL);
    settings_load_all();
    CHECK(settings_get_u8(SETTINGS_REC_QUALITY) == 0u); // 回默认
    CHECK(settings_get_u8(SETTINGS_BRIGHTNESS) == 100u);
    CHECK(settings_get_u8(SETTINGS_SCREEN_OFF_S) == 10u);
    CHECK(settings_get_float(SETTINGS_MIC_GAIN_DB) == 33.0f);
    CHECK(settings_last_save_ok()); // load 拒绝不改 save 标志
}

static void test_load_valid_extreme_kept(void)
{
    backend_reset();
    s_mem_len[SETTINGS_SCREEN_OFF_S] = 1;
    s_mem[SETTINGS_SCREEN_OFF_S][0] = 60u; // 离散最大档
    s_mem_len[SETTINGS_BRIGHTNESS] = 1;
    s_mem[SETTINGS_BRIGHTNESS][0] = 0u;
    settings_init(&s_store, NULL);
    settings_load_all();
    CHECK(settings_get_u8(SETTINGS_SCREEN_OFF_S) == 60u);
    CHECK(settings_get_u8(SETTINGS_BRIGHTNESS) == 0u);
}

static void test_reset_to_defaults(void)
{
    backend_reset();
    settings_init(&s_store, NULL);
    settings_set_u8(SETTINGS_BRIGHTNESS, 40u);
    settings_reset_to_defaults();
    CHECK(settings_get_u8(SETTINGS_BRIGHTNESS) == 100u);
}

int main(void)
{
    RUN_TEST(test_defaults_match_current_behavior);
    RUN_TEST(test_set_get_roundtrip);
    RUN_TEST(test_persist_and_reload);
    RUN_TEST(test_load_missing_key_keeps_default);
    RUN_TEST(test_load_short_value_rejected);
    RUN_TEST(test_save_failure_keeps_runtime);
    RUN_TEST(test_null_store_runtime_only);
    RUN_TEST(test_invalid_id_guarded);
    RUN_TEST(test_set_u8_out_of_range_rejected);
    RUN_TEST(test_set_gain_range_and_finite);
    RUN_TEST(test_load_garbage_values_rejected);
    RUN_TEST(test_load_valid_extreme_kept);
    RUN_TEST(test_reset_to_defaults);
    return test_report();
}
