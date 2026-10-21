// keyd 按键状态机宿主单测（M5.2）。
// 确定性回放：时间戳手动注入，模拟 10ms 采样周期（任务约定）。
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../main/sys/keyd.h"
#include "test.h"

#define SAMPLE_MS 10u

static keyd_t s_boot;
static keyd_t s_pwr;

static void reset_all(void)
{
    keyd_init(&s_boot, NULL);
    keyd_init(&s_pwr, NULL);
}

// 以 10ms 步进喂 pressed 电平 steps 次，返回最后一次喂入的事件
// （用例设计为中间 tick 无事件或已由逐 tick 断言覆盖）。
static keyd_event_t feed_ticks(keyd_t *key, bool pressed, int steps, uint32_t *now)
{
    keyd_event_t ev = KEYD_EVENT_NONE;
    for (int i = 0; i < steps; i++) {
        *now += SAMPLE_MS;
        ev = keyd_feed(key, pressed, *now);
    }
    return ev;
}

// 逐 tick 喂入并断言全程无事件（阈值扫掠用）
static void feed_expect_none(keyd_t *key, bool pressed, int steps, uint32_t *now)
{
    for (int i = 0; i < steps; i++) {
        *now += SAMPLE_MS;
        CHECK(keyd_feed(key, pressed, *now) == KEYD_EVENT_NONE);
    }
}

// 一次完整"稳定按下→稳定释放"（20ms 稳定按下 + 20ms 稳定释放）
static keyd_event_t press_and_release(keyd_t *key, uint32_t *now)
{
    feed_ticks(key, true, 2, now);
    return feed_ticks(key, false, 2, now);
}

// 释放后推进过双击窗口，返回窗口到期发出的单击事件
static keyd_event_t expire_click(keyd_t *key, uint32_t *now)
{
    for (int i = 0; i < 100; i++) {
        *now += SAMPLE_MS;
        keyd_event_t ev = keyd_feed(key, false, *now);
        if (ev != KEYD_EVENT_NONE) {
            return ev;
        }
    }
    return KEYD_EVENT_NONE;
}

static void test_debounce_jitter_under_20ms_no_event(void)
{
    reset_all();
    uint32_t now = 0;
    // 10ms 单采样抖动（1 次低电平未达 2 次稳定线），两次抖动
    now += SAMPLE_MS;
    CHECK(keyd_feed(&s_boot, true, now) == KEYD_EVENT_NONE);
    feed_ticks(&s_boot, false, 1, &now);
    feed_ticks(&s_boot, true, 1, &now);
    feed_ticks(&s_boot, false, 2, &now);
    CHECK(s_boot.stable_down == false);
}

static void test_debounce_stable_20ms_press_registers(void)
{
    reset_all();
    uint32_t now = 0;
    feed_ticks(&s_boot, true, 2, &now); // 连续 2 次低电平 = 20ms → 稳定按下
    CHECK(s_boot.stable_down == true);
}

static void test_single_click_emits_after_double_gap_expires(void)
{
    reset_all();
    uint32_t now = 0;
    press_and_release(&s_boot, &now); // release_ms=now
    feed_expect_none(&s_boot, false, 30, &now); // 窗口 300ms 内（含）无事件
    now += SAMPLE_MS;                           // now-release=310 > 300 → CLICK
    CHECK(keyd_feed(&s_boot, false, now) == KEYD_EVENT_CLICK);
}

static void test_double_click_within_window(void)
{
    reset_all();
    uint32_t now = 0;
    press_and_release(&s_boot, &now);
    feed_ticks(&s_boot, false, 5, &now); // 50ms 后窗口内第二击
    feed_ticks(&s_boot, true, 2, &now);
    CHECK(feed_ticks(&s_boot, false, 2, &now) == KEYD_EVENT_DOUBLE_CLICK);
    feed_expect_none(&s_boot, false, 60, &now); // 双击后不再补发单击
}

static void test_double_click_outside_window_two_clicks(void)
{
    reset_all();
    uint32_t now = 0;
    press_and_release(&s_boot, &now);
    CHECK(expire_click(&s_boot, &now) == KEYD_EVENT_CLICK); // 窗口到期
    press_and_release(&s_boot, &now);
    CHECK(expire_click(&s_boot, &now) == KEYD_EVENT_CLICK);
}

static void test_triple_click_sequence_click_double_click(void)
{
    reset_all();
    uint32_t now = 0;
    press_and_release(&s_boot, &now);
    feed_ticks(&s_boot, true, 2, &now); // 窗口内第二击
    CHECK(feed_ticks(&s_boot, false, 2, &now) == KEYD_EVENT_DOUBLE_CLICK);
    // 第三击（紧随第二击，成为新的双击候选第一击）→ 窗口到期出 CLICK
    press_and_release(&s_boot, &now);
    CHECK(expire_click(&s_boot, &now) == KEYD_EVENT_CLICK);
}

static void test_double_click_boundary_equal_gap_counts(void)
{
    reset_all();
    uint32_t now = 0;
    press_and_release(&s_boot, &now); // release_ms=now
    // 推进到恰好 now-release==300 时按下第二击（含边界判双击）
    feed_ticks(&s_boot, false, 28, &now); // now=rel+280；下两 tick 稳定按下于 +300
    feed_ticks(&s_boot, true, 2, &now);
    CHECK(feed_ticks(&s_boot, false, 2, &now) == KEYD_EVENT_DOUBLE_CLICK);
}

static void test_long_press_1499ms_no_event(void)
{
    reset_all();
    uint32_t now = 0;
    feed_ticks(&s_boot, true, 2, &now); // now=20 稳定按下，press_ms=20
    feed_expect_none(&s_boot, true, 149, &now); // now=1510，held=1490<1500 全程无事件
    CHECK(now == 1510u);
}

static void test_long_press_1500ms_boundary_fires(void)
{
    reset_all();
    uint32_t now = 0;
    feed_ticks(&s_boot, true, 2, &now); // press_ms=20
    feed_expect_none(&s_boot, true, 149, &now); // now=1510，held=1490
    now += SAMPLE_MS;                           // held=1500 ≥ 阈值（含边界）→ 即发
    CHECK(keyd_feed(&s_boot, true, now) == KEYD_EVENT_LONG_PRESS_1_5S);
    feed_expect_none(&s_boot, true, 50, &now); // 持续按住不重复发
    feed_ticks(&s_boot, false, 2, &now);       // 长按后松开：无 click
    feed_expect_none(&s_boot, false, 60, &now);
}

static void test_long_press_45s_warning_fires_once(void)
{
    reset_all();
    uint32_t now = 0;
    feed_ticks(&s_boot, true, 2, &now); // press_ms=20
    feed_expect_none(&s_boot, true, 149, &now); // now=1510
    now += SAMPLE_MS;                           // held=1500 → 1.5s
    CHECK(keyd_feed(&s_boot, true, now) == KEYD_EVENT_LONG_PRESS_1_5S);
    // 扫掠到 held=4490（下一 tick held=4500，含边界 → 4.5s）
    while (now - s_boot.press_ms + SAMPLE_MS < 4500) {
        now += SAMPLE_MS;
        CHECK(keyd_feed(&s_boot, true, now) == KEYD_EVENT_NONE);
    }
    now += SAMPLE_MS;
    CHECK(keyd_feed(&s_boot, true, now) == KEYD_EVENT_LONG_PRESS_4_5S);
    feed_expect_none(&s_boot, true, 50, &now); // 继续按住不重发
    feed_ticks(&s_boot, false, 2, &now);       // 松开后无 click
    feed_expect_none(&s_boot, false, 60, &now);
}

static void test_long_press_then_click_still_works(void)
{
    reset_all();
    uint32_t now = 0;
    feed_ticks(&s_boot, true, 2, &now); // press_ms=20
    feed_expect_none(&s_boot, true, 149, &now); // now=1510，held=1490
    now += SAMPLE_MS;                           // held=1500 → 即发
    CHECK(keyd_feed(&s_boot, true, now) == KEYD_EVENT_LONG_PRESS_1_5S);
    feed_ticks(&s_boot, false, 2, &now); // 长按后释放
    feed_expect_none(&s_boot, false, 60, &now);
    press_and_release(&s_boot, &now); // 之后正常单击仍可用
    CHECK(expire_click(&s_boot, &now) == KEYD_EVENT_CLICK);
}

static void test_two_instances_independent(void)
{
    reset_all();
    uint32_t now = 0;
    feed_ticks(&s_boot, true, 2, &now); // BOOT 稳定按下保持不动
    press_and_release(&s_pwr, &now);    // PWR 独立做一次单击
    CHECK(expire_click(&s_pwr, &now) == KEYD_EVENT_CLICK);
    // BOOT 未受 PWR 影响，且远未到长按阈值
    CHECK(s_boot.state == 1);
    CHECK(s_boot.lp15_fired == false);
}

static void test_reset_clears_state(void)
{
    reset_all();
    uint32_t now = 0;
    feed_ticks(&s_boot, true, 2, &now); // 按下中复位
    keyd_reset(&s_boot);
    CHECK(s_boot.stable_down == false);
    CHECK(s_boot.state == 0);
    press_and_release(&s_boot, &now); // 复位后完整单击流程正常
    CHECK(expire_click(&s_boot, &now) == KEYD_EVENT_CLICK);
}

static void test_jitter_during_hold_does_not_retrigger(void)
{
    reset_all();
    uint32_t now = 0;
    feed_ticks(&s_boot, true, 2, &now);  // 稳定按下 press_ms=20
    feed_ticks(&s_boot, false, 1, &now); // 1 次高电平抖动（未达稳定线）
    feed_ticks(&s_boot, true, 5, &now);  // 恢复低电平
    // 仍视为同一次按下：press_ms 不变，无重复事件
    CHECK(s_boot.stable_down == true);
    CHECK(s_boot.press_ms == 20u);
    feed_expect_none(&s_boot, true, 100, &now);
}

int main(void)
{
    RUN_TEST(test_debounce_jitter_under_20ms_no_event);
    RUN_TEST(test_debounce_stable_20ms_press_registers);
    RUN_TEST(test_single_click_emits_after_double_gap_expires);
    RUN_TEST(test_double_click_within_window);
    RUN_TEST(test_double_click_outside_window_two_clicks);
    RUN_TEST(test_triple_click_sequence_click_double_click);
    RUN_TEST(test_double_click_boundary_equal_gap_counts);
    RUN_TEST(test_long_press_1499ms_no_event);
    RUN_TEST(test_long_press_1500ms_boundary_fires);
    RUN_TEST(test_long_press_45s_warning_fires_once);
    RUN_TEST(test_long_press_then_click_still_works);
    RUN_TEST(test_two_instances_independent);
    RUN_TEST(test_reset_clears_state);
    RUN_TEST(test_jitter_during_hold_does_not_retrigger);
    return test_report();
}
