// beep 提示音序列表宿主单测（M5.2）：事件数/音数/时长/间隔/频率表正确性。
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../main/sys/beep.h"
#include "test.h"

static void test_rec_start_one_short(void)
{
    uint8_t n = 0;
    const beep_tone_t *t = beep_get_sequence(BEEP_EVT_REC_START, &n);
    CHECK(n == 1);
    CHECK(t != NULL);
    CHECK(t[0].freq_hz == BEEP_FREQ_SHORT_HZ);
    CHECK(t[0].dur_ms == BEEP_DUR_SHORT_MS);
    CHECK(beep_sequence_duration_ms(BEEP_EVT_REC_START) == BEEP_DUR_SHORT_MS);
}

static void test_rec_stop_two_shorts_with_gap(void)
{
    uint8_t n = 0;
    const beep_tone_t *t = beep_get_sequence(BEEP_EVT_REC_STOP, &n);
    CHECK(n == 2);
    CHECK(t[0].freq_hz == BEEP_FREQ_SHORT_HZ && t[1].freq_hz == BEEP_FREQ_SHORT_HZ);
    CHECK(beep_sequence_duration_ms(BEEP_EVT_REC_STOP) == 2 * BEEP_DUR_SHORT_MS + BEEP_GAP_MS);
}

static void test_pause_one_long(void)
{
    uint8_t n = 0;
    const beep_tone_t *t = beep_get_sequence(BEEP_EVT_PAUSE, &n);
    CHECK(n == 1);
    CHECK(t[0].freq_hz == BEEP_FREQ_LONG_HZ);
    CHECK(t[0].dur_ms == BEEP_DUR_LONG_MS);
}

static void test_resume_one_short(void)
{
    uint8_t n = 0;
    const beep_tone_t *t = beep_get_sequence(BEEP_EVT_RESUME, &n);
    CHECK(n == 1);
    CHECK(t[0].freq_hz == BEEP_FREQ_SHORT_HZ && t[0].dur_ms == BEEP_DUR_SHORT_MS);
}

static void test_mark_extremely_short(void)
{
    uint8_t n = 0;
    const beep_tone_t *t = beep_get_sequence(BEEP_EVT_MARK, &n);
    CHECK(n == 1);
    CHECK(t[0].dur_ms == BEEP_DUR_MARK_MS);
    CHECK(t[0].dur_ms < BEEP_DUR_SHORT_MS); // 极短 < 短，听感可辨
}

static void test_error_three_shorts(void)
{
    uint8_t n = 0;
    (void)beep_get_sequence(BEEP_EVT_ERROR, &n);
    CHECK(n == 3);
    CHECK(beep_sequence_duration_ms(BEEP_EVT_ERROR) == 3 * BEEP_DUR_SHORT_MS + 2 * BEEP_GAP_MS);
}

static void test_connected_rising_two_tones(void)
{
    uint8_t n = 0;
    const beep_tone_t *t = beep_get_sequence(BEEP_EVT_CONNECTED, &n);
    CHECK(n == 2);
    CHECK(t[0].freq_hz == BEEP_FREQ_LONG_HZ); // 660
    CHECK(t[1].freq_hz == BEEP_FREQ_HIGH_HZ); // 990 上扬
    CHECK(t[1].freq_hz > t[0].freq_hz);
    CHECK(beep_sequence_duration_ms(BEEP_EVT_CONNECTED) == 2 * BEEP_DUR_RISE_MS + BEEP_GAP_MS);
}

static void test_invalid_event_returns_null(void)
{
    uint8_t n = 99;
    CHECK(beep_get_sequence(BEEP_EVT_COUNT, &n) == NULL);
    CHECK(n == 0);
    CHECK(beep_sequence_duration_ms(BEEP_EVT_COUNT) == 0);
}

static void test_enabled_switch_default_on(void)
{
    CHECK(beep_is_enabled() == true); // 默认开（单测进程内首个断言；无前置关闭）
    beep_set_enabled(false);
    CHECK(beep_is_enabled() == false);
    beep_set_enabled(true);
    CHECK(beep_is_enabled() == true);
}

int main(void)
{
    RUN_TEST(test_rec_start_one_short);
    RUN_TEST(test_rec_stop_two_shorts_with_gap);
    RUN_TEST(test_pause_one_long);
    RUN_TEST(test_resume_one_short);
    RUN_TEST(test_mark_extremely_short);
    RUN_TEST(test_error_three_shorts);
    RUN_TEST(test_connected_rising_two_tones);
    RUN_TEST(test_invalid_event_returns_null);
    RUN_TEST(test_enabled_switch_default_on);
    return test_report();
}
