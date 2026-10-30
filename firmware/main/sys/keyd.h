// Beecord 按键状态机（keyd.h，M5.2）。
// 硬件无关纯逻辑：电平采样由调用者按固定周期喂入（esp_* 由 keyd_gpio 侧承担），
// 时间戳由调用者注入（毫秒，单调递增即可），方便宿主单测做确定性回放。
// 语义（ux-device.md §1 按键映射）：
//   单击 click / 双击 double_click（窗口内第二击）/ 长按 1.5s（mark）/
//   长按 4.5s（关机警告，为 PWR ~6s 硬件断电预留软件告警窗口）；
//   长按事件到阈值即发、不等松开；长按后松开不再产生 click。
// 消抖沿用 M5.1 语义：调用者 10ms 采样，连续 N 次同电平判稳定（默认 2 次 = 20ms）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    KEYD_EVENT_NONE = 0,
    KEYD_EVENT_CLICK,            // 单击（释放后经双击窗口确认无第二击）
    KEYD_EVENT_DOUBLE_CLICK,     // 双击（窗口 ≤300ms 内第二击释放时发出）
    KEYD_EVENT_LONG_PRESS_1_5S,  // 长按 1.5s（到阈值即发）
    KEYD_EVENT_LONG_PRESS_4_5S,  // 长按 4.5s（关机警告，到阈值即发）
} keyd_event_t;

// 多键实例的键位标识（keyd_gpio 绑定与 UI 分发用）
typedef enum {
    KEYD_KEY_BOOT = 0, // 功能键（GPIO0，低=按下）
    KEYD_KEY_PWR,      // 电源键（GPIO10，高=按下，M5.2 实机验证）
    KEYD_KEY_COUNT,
} keyd_key_id_t;

typedef struct {
    uint8_t debounce_samples;     // 消抖采样数（10ms 周期下 2 = 20ms）
    uint16_t double_click_gap_ms; // 双击窗口（从第一击释放算起，含边界）
    uint16_t long_press_ms;       // 长按 1.5s 阈值
    uint16_t shutdown_warn_ms;    // 长按 4.5s 关机警告阈值
} keyd_config_t;

typedef struct {
    keyd_config_t cfg;
    bool raw_prev;      // 上次原始电平
    uint8_t same_cnt;   // 连续同电平采样计数
    bool stable_down;   // 消抖后的稳定电平（true=按下）
    uint8_t state;      // 内部状态：0=空闲 1=稳定按下 2=待双击判定
    uint32_t press_ms;  // 稳定按下起始时间
    uint32_t release_ms;// 稳定释放起始时间（双击窗口判定基准）
    bool lp15_fired;    // 1.5s 长按已发（防重复）
    bool lp45_fired;    // 4.5s 警告已发（防重复）
    bool dbl_second;    // 当前按下为双击第二击（窗口内到达）
} keyd_t;

// 初始化（cfg 传 NULL 用默认：2 采样/300ms/1500ms/4500ms）
void keyd_init(keyd_t *key, const keyd_config_t *cfg);

// 复位到空闲态（不断电场景/测试用）
void keyd_reset(keyd_t *key);

// 喂入一次电平采样。pressed=原始电平（true=按下），now_ms=当前时间戳。
// 返回本次喂入产生的事件（每次调用至多一个事件；NONE 表示无事件）。
// 调用方按固定周期（默认 10ms）调用；窗口/阈值判定在喂入时进行，
// 因此事件延迟 ≤ 一个采样周期。
keyd_event_t keyd_feed(keyd_t *key, bool pressed, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
