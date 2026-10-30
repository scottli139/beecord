// power 电源逻辑宿主单测（M5.4 二阶段）：EMA 平滑、低电连续样本确认、
// 迟滞解除、充电抑制、非法样本跳过、未知态。依据 ux-device §7。
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "../main/sys/power.h"
#include "test.h"

static power_t s_p;

static void feed_n(float pct, bool charging, int n)
{
    for (int i = 0; i < n; i++) {
        (void)power_feed_sample(&s_p, pct, charging);
    }
}

static void test_unknown_until_first_sample(void)
{
    power_init(&s_p, NULL);
    CHECK(power_display_pct(&s_p) == POWER_PCT_UNKNOWN);
    CHECK(!power_is_known(&s_p));
    CHECK(!power_is_low(&s_p));
}

static void test_first_sample_direct_and_smooth(void)
{
    power_init(&s_p, NULL);
    (void)power_feed_sample(&s_p, 50.0f, false);
    CHECK(power_display_pct(&s_p) == 50u); // 首样本直取，不从 0 爬坡
    // α=0.2：50→80 → 50+30*0.2=56
    (void)power_feed_sample(&s_p, 80.0f, false);
    CHECK(power_display_pct(&s_p) == 56u);
}

static void test_low_requires_consecutive_samples(void)
{
    power_init(&s_p, NULL);
    feed_n(60.0f, false, 5); // 常态
    // 单点低（抖动）不触发
    uint8_t ev = power_feed_sample(&s_p, 5.0f, false);
    CHECK((ev & POWER_EVT_LOW_ON) == 0u);
    CHECK(!power_is_low(&s_p));
    feed_n(60.0f, false, 2); // 回到常态：low_count 清零
    // 连续 3 个低样本（默认 low_confirm_n=3）：EMA 钝化后仍 ≤10
    ev = 0;
    ev |= power_feed_sample(&s_p, 5.0f, false);
    ev |= power_feed_sample(&s_p, 5.0f, false);
    ev |= power_feed_sample(&s_p, 5.0f, false);
    CHECK((ev & POWER_EVT_LOW_ON) != 0u);
    CHECK(power_is_low(&s_p));
}

static void test_charging_suppresses_low(void)
{
    power_init(&s_p, NULL);
    uint8_t ev = 0;
    for (int i = 0; i < 10; i++) {
        ev |= power_feed_sample(&s_p, 5.0f, true); // 低电量但充电中
    }
    CHECK((ev & POWER_EVT_LOW_ON) == 0u);
    CHECK(!power_is_low(&s_p));
}

static void test_hysteresis_release(void)
{
    power_init(&s_p, NULL);
    feed_n(5.0f, false, 5); // 触发低电
    CHECK(power_is_low(&s_p));
    // 回升到 11%（触发 10 与解除 13 之间的迟滞带）：不解除
    uint8_t ev = 0;
    for (int i = 0; i < 8; i++) {
        ev |= power_feed_sample(&s_p, 11.0f, false);
    }
    CHECK((ev & POWER_EVT_LOW_OFF) == 0u);
    CHECK(power_is_low(&s_p));
    // 回到 5%（迟滞带内下行）：仍保持
    for (int i = 0; i < 3; i++) {
        (void)power_feed_sample(&s_p, 5.0f, false);
    }
    CHECK(power_is_low(&s_p));
    // 连续 5 个 ≥13%（默认 release_n=5，EMA 上升后）→ 解除
    ev = 0;
    for (int i = 0; i < 10; i++) {
        ev |= power_feed_sample(&s_p, 60.0f, false);
    }
    CHECK((ev & POWER_EVT_LOW_OFF) != 0u);
    CHECK(!power_is_low(&s_p));
}

static void test_invalid_samples_skipped(void)
{
    power_init(&s_p, NULL);
    (void)power_feed_sample(&s_p, 50.0f, false);
    CHECK(power_feed_sample(&s_p, -1.0f, false) == POWER_EVT_NONE);
    CHECK(power_feed_sample(&s_p, 101.0f, false) == POWER_EVT_NONE);
    CHECK(power_feed_sample(&s_p, sqrtf(-1.0f), false) == POWER_EVT_NONE); // NaN
    CHECK(power_display_pct(&s_p) == 50u); // 状态未被污染
}

static void test_display_clamps(void)
{
    power_init(&s_p, NULL);
    (void)power_feed_sample(&s_p, 100.0f, false);
    CHECK(power_display_pct(&s_p) == 100u);
}

static void test_edge_threshold_exclusive(void)
{
    // ux-device 明确 <10%：恰好 10% 不触发
    power_init(&s_p, NULL);
    uint8_t ev = 0;
    for (int i = 0; i < 12; i++) {
        ev |= power_feed_sample(&s_p, 10.0f, false);
    }
    CHECK((ev & POWER_EVT_LOW_ON) == 0u);
    CHECK(!power_is_low(&s_p));
    // 9% 连续 3 样本触发（迟滞解除阈值 13 不受影响）
    for (int i = 0; i < 3; i++) {
        ev |= power_feed_sample(&s_p, 9.0f, false);
    }
    CHECK((ev & POWER_EVT_LOW_ON) != 0u);
    CHECK(power_is_low(&s_p));
}

int main(void)
{
    RUN_TEST(test_unknown_until_first_sample);
    RUN_TEST(test_first_sample_direct_and_smooth);
    RUN_TEST(test_low_requires_consecutive_samples);
    RUN_TEST(test_charging_suppresses_low);
    RUN_TEST(test_hysteresis_release);
    RUN_TEST(test_invalid_samples_skipped);
    RUN_TEST(test_display_clamps);
    RUN_TEST(test_edge_threshold_exclusive);
    return test_report();
}
