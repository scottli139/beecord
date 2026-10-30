// Beecord 提示音序列逻辑（beep.h，M5.2）。
// 纯逻辑：事件 → 音序列（频率/时长表）的静态映射，禁 esp_* 头（宿主可测）。
// 事件表对齐 ux-device.md §8 声音反馈表；参数为取悦耳默认（可后续调）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BEEP_EVT_REC_START = 0, // 开始录音：滴（1 短）
    BEEP_EVT_REC_STOP,      // 停止并保存：滴滴（2 短）
    BEEP_EVT_PAUSE,         // 暂停：滴—（1 长）
    BEEP_EVT_RESUME,        // 继续：滴（1 短）
    BEEP_EVT_MARK,          // 打标记点：嘀（极短）
    BEEP_EVT_ERROR,         // 低电自动停止/错误：滴滴滴（3 短）
    BEEP_EVT_CONNECTED,     // 连接 Mac 成功：滴-滴（2 音上扬 660→990Hz）
    BEEP_EVT_COUNT,
} beep_event_t;

typedef struct {
    uint16_t freq_hz; // 正弦频率
    uint16_t dur_ms;  // 音长
} beep_tone_t;

// 音间间隔（固定全表一致）
#define BEEP_GAP_MS 60u

// 取悦耳默认参数（M5.2 初版，实机听辨后可调）
#define BEEP_FREQ_SHORT_HZ 880u   // 短音
#define BEEP_FREQ_LONG_HZ 660u    // 长音
#define BEEP_FREQ_HIGH_HZ 990u    // 扬了上去的第二音
#define BEEP_DUR_SHORT_MS 80u
#define BEEP_DUR_LONG_MS 300u
#define BEEP_DUR_MARK_MS 40u
#define BEEP_DUR_RISE_MS 120u

// 取事件的音序列（静态表，生命周期恒有效）；音数写入 out_count。
// evt 非法时返回 NULL 且 out_count=0。
const beep_tone_t *beep_get_sequence(beep_event_t evt, uint8_t *out_count);

// 事件序列总时长（音长之和 + 音间间隔），ms；用于调度/测试断言。
// 注：不含播放层前导/收尾静音（BEEP_LEAD_IN_MS/BEEP_TAIL_MS，beep_play.c），
// 该预算只描述音序列表本身。
uint32_t beep_sequence_duration_ms(beep_event_t evt);

// 全局开关（M5.4 起经 settings NVS 持久化；默认开）。关闭时播放层静默。
void beep_set_enabled(bool enabled);
bool beep_is_enabled(void);

#ifdef __cplusplus
}
#endif
