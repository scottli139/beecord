// rec_meter 电平统计宿主单测（M5.3）：RMS→24 档映射与平滑语义。
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../main/audio/rec_meter.h"
#include "test.h"

#define FRAME_N 320u // 与 Opus 帧等长，贴近真实喂入粒度

static int16_t s_pcm[FRAME_N];

// 生成幅值 amp 的满度正弦帧（RMS≈amp/√2）
static void fill_sine(int16_t amp)
{
    for (uint32_t i = 0; i < FRAME_N; i++) {
        s_pcm[i] = (int16_t)(amp * sin(2.0 * 3.14159265358979 * 440.0 * (double)i / 16000.0));
    }
}

// 直流满度帧（RMS=amp，最高能量输入）
static void fill_dc(int16_t v)
{
    for (uint32_t i = 0; i < FRAME_N; i++) {
        s_pcm[i] = v;
    }
}

static void test_silence_feeds_zero(void)
{
    rec_meter_t m;
    rec_meter_init(&m, NULL);
    memset(s_pcm, 0, sizeof(s_pcm));
    for (int i = 0; i < 10; i++) {
        CHECK(rec_meter_feed(&m, s_pcm, FRAME_N) == 0);
    }
}

static void test_full_scale_reaches_max(void)
{
    rec_meter_t m;
    rec_meter_init(&m, NULL);
    fill_dc(32767); // 0dB，远超天花板 -23dB
    uint8_t lvl = 0;
    for (int i = 0; i < 20 && lvl < REC_METER_LEVEL_MAX; i++) {
        lvl = rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    CHECK(lvl == REC_METER_LEVEL_MAX);
    CHECK(rec_meter_last_target(&m) == REC_METER_LEVEL_MAX);
}

static void test_speech_range_grades(void)
{
    // M5.3 实机定标抽查（-67dB 地板 / -23dB 天花，span 44dB → 每档 ≈1.83dB）：
    //   幅值 100 正弦 RMS≈71（-53.3dB）→ ≈7 档；幅值 50（-59.3dB）→ ≈4 档；
    //   幅值 200（-47.3dB）→ ≈11 档；幅值 1000（-33.3dB）→ ≈18 档
    struct {
        int amp;
        uint8_t expect;
    } cases[] = {
        { 100, 7 }, { 50, 4 }, { 200, 11 }, { 1000, 18 },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        rec_meter_t m;
        rec_meter_init(&m, NULL);
        for (uint32_t i = 0; i < FRAME_N; i++) {
            s_pcm[i] = (int16_t)((double)cases[c].amp *
                                 sin(2.0 * 3.14159265358979 * 440.0 * (double)i / 16000.0));
        }
        uint8_t lvl = 0;
        for (int i = 0; i < 20; i++) {
            lvl = rec_meter_feed(&m, s_pcm, FRAME_N);
        }
        CHECK(lvl >= cases[c].expect - 1u && lvl <= cases[c].expect + 1u);
    }
}

static void test_noise_floor_below_floor_maps_zero(void)
{
    // 底噪 rms≈15（-66.8dB）贴地板 → 0 档（压噪目标）
    rec_meter_t m;
    rec_meter_init(&m, NULL);
    for (uint32_t i = 0; i < FRAME_N; i++) {
        s_pcm[i] = (int16_t)(15 * ((i % 2) ? 1 : -1)); // rms≈15
    }
    uint8_t lvl = 0;
    for (int i = 0; i < 20; i++) {
        lvl = rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    CHECK(lvl <= 1u);
}

static void test_half_amplitude_saturates(void)
{
    // 半幅值 = -6.02dB，高于天花板 -23dB → 顶格（动态段在 -67~-23dB 之间）
    rec_meter_t m;
    rec_meter_init(&m, NULL);
    fill_dc(16384);
    uint8_t lvl = 0;
    for (int i = 0; i < 20; i++) {
        lvl = rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    CHECK(lvl == REC_METER_LEVEL_MAX);
}

static void test_quarter_amplitude_saturates(void)
{
    // 1/4 幅值 = -12.04dB，仍高于天花板 → 顶格
    rec_meter_t m;
    rec_meter_init(&m, NULL);
    fill_dc(8192);
    uint8_t lvl = 0;
    for (int i = 0; i < 20; i++) {
        lvl = rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    CHECK(lvl == REC_METER_LEVEL_MAX);
}

static void test_attack_faster_than_decay(void)
{
    rec_meter_t m;
    rec_meter_init(&m, NULL);
    fill_dc(32767);
    uint8_t up = 0;
    for (int i = 0; i < 20; i++) {
        up = rec_meter_feed(&m, s_pcm, FRAME_N);
        if (up == REC_METER_LEVEL_MAX) {
            break;
        }
    }
    CHECK(up == REC_METER_LEVEL_MAX);
    // 静默：每帧至多退 decay_per_feed(1) 档
    memset(s_pcm, 0, sizeof(s_pcm));
    CHECK(rec_meter_feed(&m, s_pcm, FRAME_N) == REC_METER_LEVEL_MAX - 1);
    CHECK(rec_meter_feed(&m, s_pcm, FRAME_N) == REC_METER_LEVEL_MAX - 2);
    CHECK(rec_meter_feed(&m, s_pcm, FRAME_N) == REC_METER_LEVEL_MAX - 3);
}

static void test_empty_feed_only_decays(void)
{
    rec_meter_t m;
    rec_meter_init(&m, NULL);
    fill_dc(32767);
    uint8_t lvl = 0;
    for (int i = 0; i < 20; i++) {
        lvl = rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    CHECK(lvl == REC_METER_LEVEL_MAX);
    CHECK(rec_meter_feed(&m, NULL, 0) == REC_METER_LEVEL_MAX - 1);
}

static void test_reset_zeroes_level(void)
{
    rec_meter_t m;
    rec_meter_init(&m, NULL);
    fill_dc(32767);
    for (int i = 0; i < 20; i++) {
        (void)rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    rec_meter_reset(&m);
    CHECK(m.level == 0);
}

static void test_frame_size_independent(void)
{
    // 同能量密度不同帧长 → 同目标档（RMS 与帧长无关）
    rec_meter_t m1, m2;
    rec_meter_init(&m1, NULL);
    rec_meter_init(&m2, NULL);
    static int16_t big[1600];
    for (uint32_t i = 0; i < 1600; i++) {
        big[i] = 16384;
    }
    for (int i = 0; i < 20; i++) {
        (void)rec_meter_feed(&m1, big, 1600);
        (void)rec_meter_feed(&m2, big, 160);
    }
    CHECK(m1.level == m2.level);
}

static void test_decay_per_feed_config(void)
{
    rec_meter_config_t cfg = { .decay_per_feed = 3 };
    rec_meter_t m;
    rec_meter_init(&m, &cfg);
    fill_dc(32767);
    for (int i = 0; i < 20; i++) {
        (void)rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    CHECK(m.level == REC_METER_LEVEL_MAX);
    memset(s_pcm, 0, sizeof(s_pcm));
    CHECK(rec_meter_feed(&m, s_pcm, FRAME_N) == REC_METER_LEVEL_MAX - 3);
}

static void test_window_target_getter(void)
{
    // rec_meter_last_target 镜像最近块未平滑目标；reset 清零
    rec_meter_t m;
    rec_meter_init(&m, NULL);
    CHECK(rec_meter_last_target(&m) == 0);
    fill_sine(100);
    (void)rec_meter_feed(&m, s_pcm, FRAME_N);
    CHECK(rec_meter_last_target(&m) >= 5u && rec_meter_last_target(&m) <= 9u);
    rec_meter_reset(&m);
    CHECK(rec_meter_last_target(&m) == 0);
}



static void test_null_meter_safe(void)
{
    fill_dc(100);
    CHECK(rec_meter_feed(NULL, s_pcm, FRAME_N) == 0);
    rec_meter_reset(NULL); // 不崩即可
    rec_meter_init(NULL, NULL);
}

static void test_attack_from_midlevel(void)
{
    rec_meter_t m;
    rec_meter_init(&m, NULL);
    fill_sine(100); // -53.3dB → ~7 档中段
    uint8_t lvl = 0;
    for (int i = 0; i < 20; i++) {
        lvl = rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    CHECK(lvl >= 6 && lvl <= 8);
    fill_dc(32767); // 跳满：上行多帧逼近
    for (int i = 0; i < 10; i++) {
        lvl = rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    CHECK(lvl == REC_METER_LEVEL_MAX);
}

static void test_decay_floors_at_zero(void)
{
    rec_meter_t m;
    rec_meter_init(&m, NULL);
    fill_dc(32767);
    for (int i = 0; i < 20; i++) {
        (void)rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    memset(s_pcm, 0, sizeof(s_pcm));
    for (int i = 0; i < 40; i++) {
        uint8_t lvl = rec_meter_feed(&m, s_pcm, FRAME_N);
        CHECK((int)lvl >= 0); // 无符号下溢检查：恒 ≥0
    }
    CHECK(m.level == 0);
}

static void test_negative_full_scale_energy(void)
{
    // -32768 平方同 +32767：能量满度 → 24 档
    rec_meter_t m;
    rec_meter_init(&m, NULL);
    for (uint32_t i = 0; i < FRAME_N; i++) {
        s_pcm[i] = -32768;
    }
    uint8_t lvl = 0;
    for (int i = 0; i < 20; i++) {
        lvl = rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    CHECK(lvl == REC_METER_LEVEL_MAX);
}

static void test_default_cfg_decay_one(void)
{
    rec_meter_t m;
    rec_meter_init(&m, NULL);
    CHECK(m.cfg.decay_per_feed == 1);
    fill_dc(32767);
    for (int i = 0; i < 20; i++) {
        (void)rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    memset(s_pcm, 0, sizeof(s_pcm));
    CHECK(rec_meter_feed(&m, s_pcm, FRAME_N) == REC_METER_LEVEL_MAX - 1);
}

// 回归：漏掉 rec_meter_init 时（cfg 全 0）feed 也必须退档——
// 实机"电平条等高冻结"根因（buzz_recorder 曾只 reset 不 init，decay=0 永不退档）
static void test_uninit_meter_still_decays(void)
{
    rec_meter_t m;
    memset(&m, 0, sizeof(m)); // 模拟静态变量未 init
    fill_dc(32767);
    for (int i = 0; i < 20; i++) {
        (void)rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    CHECK(m.level == REC_METER_LEVEL_MAX);
    memset(s_pcm, 0, sizeof(s_pcm));
    for (uint32_t i = 0; i < REC_METER_LEVEL_MAX + 2u; i++) {
        (void)rec_meter_feed(&m, s_pcm, FRAME_N);
    }
    CHECK(m.level == 0);
}

int main(void)
{
    RUN_TEST(test_silence_feeds_zero);
    RUN_TEST(test_full_scale_reaches_max);
    RUN_TEST(test_half_amplitude_saturates);
    RUN_TEST(test_quarter_amplitude_saturates);
    RUN_TEST(test_attack_faster_than_decay);
    RUN_TEST(test_empty_feed_only_decays);
    RUN_TEST(test_reset_zeroes_level);
    RUN_TEST(test_frame_size_independent);
    RUN_TEST(test_decay_per_feed_config);
    RUN_TEST(test_speech_range_grades);
    RUN_TEST(test_noise_floor_below_floor_maps_zero);
    RUN_TEST(test_window_target_getter);
    RUN_TEST(test_null_meter_safe);
    RUN_TEST(test_attack_from_midlevel);
    RUN_TEST(test_decay_floors_at_zero);
    RUN_TEST(test_negative_full_scale_energy);
    RUN_TEST(test_default_cfg_decay_one);
    RUN_TEST(test_uninit_meter_still_decays);
    return test_report();
}
