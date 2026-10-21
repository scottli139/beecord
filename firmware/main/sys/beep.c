// Beecord 提示音序列逻辑实现（beep.c，M5.2）。
#include "beep.h"

#include <stddef.h>

// 静态音序列表（ux-device.md §8）：顺序即播放顺序
static const beep_tone_t s_seq_start[] = {{BEEP_FREQ_SHORT_HZ, BEEP_DUR_SHORT_MS}};
static const beep_tone_t s_seq_stop[] = {
    {BEEP_FREQ_SHORT_HZ, BEEP_DUR_SHORT_MS},
    {BEEP_FREQ_SHORT_HZ, BEEP_DUR_SHORT_MS},
};
static const beep_tone_t s_seq_pause[] = {{BEEP_FREQ_LONG_HZ, BEEP_DUR_LONG_MS}};
static const beep_tone_t s_seq_resume[] = {{BEEP_FREQ_SHORT_HZ, BEEP_DUR_SHORT_MS}};
static const beep_tone_t s_seq_mark[] = {{BEEP_FREQ_SHORT_HZ, BEEP_DUR_MARK_MS}};
static const beep_tone_t s_seq_error[] = {
    {BEEP_FREQ_SHORT_HZ, BEEP_DUR_SHORT_MS},
    {BEEP_FREQ_SHORT_HZ, BEEP_DUR_SHORT_MS},
    {BEEP_FREQ_SHORT_HZ, BEEP_DUR_SHORT_MS},
};
static const beep_tone_t s_seq_connected[] = {
    {BEEP_FREQ_LONG_HZ, BEEP_DUR_RISE_MS},
    {BEEP_FREQ_HIGH_HZ, BEEP_DUR_RISE_MS},
};

// 事件 → 序列映射表（播放层也用它做总时长预算）
typedef struct {
    const beep_tone_t *tones;
    uint8_t count;
} beep_seq_entry_t;

static const beep_seq_entry_t s_table[BEEP_EVT_COUNT] = {
    [BEEP_EVT_REC_START] = {s_seq_start, 1},
    [BEEP_EVT_REC_STOP] = {s_seq_stop, 2},
    [BEEP_EVT_PAUSE] = {s_seq_pause, 1},
    [BEEP_EVT_RESUME] = {s_seq_resume, 1},
    [BEEP_EVT_MARK] = {s_seq_mark, 1},
    [BEEP_EVT_ERROR] = {s_seq_error, 3},
    [BEEP_EVT_CONNECTED] = {s_seq_connected, 2},
};

static bool s_enabled = true; // 默认开（NVS 设置项 M5.4 接入）

const beep_tone_t *beep_get_sequence(beep_event_t evt, uint8_t *out_count)
{
    if (out_count != NULL) {
        *out_count = 0;
    }
    if ((int)evt < 0 || evt >= BEEP_EVT_COUNT || out_count == NULL) {
        return NULL;
    }
    *out_count = s_table[evt].count;
    return s_table[evt].tones;
}

uint32_t beep_sequence_duration_ms(beep_event_t evt)
{
    if ((int)evt < 0 || evt >= BEEP_EVT_COUNT) {
        return 0;
    }
    uint32_t total = 0;
    for (uint8_t i = 0; i < s_table[evt].count; i++) {
        total += s_table[evt].tones[i].dur_ms;
    }
    if (s_table[evt].count > 1) {
        total += BEEP_GAP_MS * (s_table[evt].count - 1); // 音间间隔
    }
    return total;
}

void beep_set_enabled(bool enabled)
{
    s_enabled = enabled;
}

bool beep_is_enabled(void)
{
    return s_enabled;
}
