// Beecord 按键状态机实现（keyd.c，M5.2）。
// 设计要点：消抖与状态时序分离——电平经"连续 N 次同值"滤波后才参与状态机，
// 所有时间判定（双击窗口/长按阈值）都在喂入时以注入时间戳计算，无真实定时器依赖。
#include "keyd.h"

#include <stddef.h>

#define KEYD_STATE_IDLE 0      // 稳定释放（或长按后释放）
#define KEYD_STATE_DOWN 1      // 稳定按下（第一击 / 双击第二击 / 普通长按）
#define KEYD_STATE_WAIT_DBL 2  // 已释放，等待双击窗口到期（期内再按下转双击）

static const keyd_config_t s_default_cfg = {
    .debounce_samples = 2,
    .double_click_gap_ms = 300,
    .long_press_ms = 1500,
    .shutdown_warn_ms = 4500,
};

void keyd_init(keyd_t *key, const keyd_config_t *cfg)
{
    if (key == NULL) {
        return;
    }
    key->cfg = (cfg != NULL) ? *cfg : s_default_cfg;
    if (key->cfg.debounce_samples == 0) {
        key->cfg.debounce_samples = 1; // 至少一次采样即稳定
    }
    keyd_reset(key);
}

void keyd_reset(keyd_t *key)
{
    key->raw_prev = false;
    key->same_cnt = 0;
    key->stable_down = false;
    key->state = KEYD_STATE_IDLE;
    key->press_ms = 0;
    key->release_ms = 0;
    key->lp15_fired = false;
    key->lp45_fired = false;
    key->dbl_second = false;
}

keyd_event_t keyd_feed(keyd_t *key, bool pressed, uint32_t now_ms)
{
    if (key == NULL) {
        return KEYD_EVENT_NONE;
    }

    // ---- 电平消抖：连续 N 次同值才翻转稳定电平 ----
    if (pressed == key->raw_prev) {
        key->same_cnt++;
    } else {
        key->raw_prev = pressed;
        key->same_cnt = 1;
    }
    bool stable = key->same_cnt >= key->cfg.debounce_samples;
    bool new_stable_down = stable ? pressed : key->stable_down;
    bool edge = stable && (new_stable_down != key->stable_down);
    key->stable_down = new_stable_down;

    if (edge) {
        if (new_stable_down) {
            // 按下沿：双击窗口内到达 → 本次为第二击（取消待判定单击）；
            // 窗口外 → 全新一次按下
            key->dbl_second = (key->state == KEYD_STATE_WAIT_DBL) &&
                              (now_ms - key->release_ms <= key->cfg.double_click_gap_ms);
            key->state = KEYD_STATE_DOWN;
            key->press_ms = now_ms;
            key->lp15_fired = false;
            key->lp45_fired = false;
        } else {
            // 释放沿
            if (key->state == KEYD_STATE_DOWN && !key->lp15_fired) {
                if (key->dbl_second) {
                    key->state = KEYD_STATE_IDLE;
                    key->dbl_second = false;
                    return KEYD_EVENT_DOUBLE_CLICK;
                }
                key->state = KEYD_STATE_WAIT_DBL;
                key->release_ms = now_ms;
            } else {
                // 长按后释放（或异常态）：长按已发过事件，不再产生 click
                key->state = KEYD_STATE_IDLE;
                key->dbl_second = false;
            }
        }
    }

    // ---- 时序判定（每次喂入都检查阈值/窗口）----
    switch (key->state) {
    case KEYD_STATE_DOWN:
        if (!key->lp45_fired && now_ms - key->press_ms >= key->cfg.shutdown_warn_ms) {
            key->lp45_fired = true; // 4.5s 后到阈值即发（1.5s 事件已先于其发出）
            return KEYD_EVENT_LONG_PRESS_4_5S;
        }
        if (!key->lp15_fired && now_ms - key->press_ms >= key->cfg.long_press_ms) {
            key->lp15_fired = true;
            return KEYD_EVENT_LONG_PRESS_1_5S;
        }
        break;
    case KEYD_STATE_WAIT_DBL:
        // 窗口到期且无第二击 → 单击（含边界：第二击在 now-release==gap 的
        // 喂入会先经 edge 分支转入 DOWN，到不了这里）
        if (now_ms - key->release_ms > key->cfg.double_click_gap_ms) {
            key->state = KEYD_STATE_IDLE;
            return KEYD_EVENT_CLICK;
        }
        break;
    default:
        break;
    }
    return KEYD_EVENT_NONE;
}
