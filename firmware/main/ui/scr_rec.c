// Beecord 录音页实现（scr_rec.c，M5.3）。
// 布局（页面 padding 20/24/30，DESIGN_SYSTEM v1.9）：
//   顶部状态行：REC 点（1200ms 呼吸脉冲）+ 计时（timer 56）
//   标记刻度条（h4 轨道 + brand.primary 2×8 刻度，按 mark/elapsed 定位）
//   24 根电平条（w6 r3 录音红，高 6%~100%）
//   暂停/继续次按钮；底部"按住停止"按住确认区（800ms 进度填充，§3）
// 电平条为 RMS 历史带：右端=当前电平，向左为近 24 次采样历史——
// 单值 RMS 不做假频谱；历史带是真实数据的诚实可视化。
#include "scr_rec.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "storage/buzz_sdcard.h"
#include "audio/rec_meter.h"
#include "screen_pwr.h"
#include "theme.h"
#include "ui_rec.h"

#define REC_BARS 24u
#define REC_BAR_W 6u
#define REC_BAR_GAP 8u
#define REC_BAR_AREA_H 120u
#define REC_HOLD_STOP_MS 800u          // §3 按住确认时长
#define REC_HOLD_FILL_OPA LV_OPA_20    // 进度填充透明度
#define REC_MARK_TICK_W 2u
#define REC_MARK_TICK_H 8u
#define REC_POLL_MS 200u
#define REC_HISTORY_N REC_BARS

static lv_obj_t *s_screen;
static lv_obj_t *s_prev_screen;
static lv_obj_t *s_rec_dot;
static lv_obj_t *s_timer_lab;
static lv_obj_t *s_pause_btn;   // 暂停/继续
static lv_obj_t *s_pause_lab;
static lv_obj_t *s_hold_area;   // 按住停止
static lv_obj_t *s_hold_fill;
static lv_obj_t *s_hold_lab;
static lv_obj_t *s_track;       // 标记刻度轨道
static lv_obj_t *s_no_card_bar;
static lv_obj_t *s_bars[REC_BARS];

static lv_timer_t *s_poll_timer;
static lv_timer_t *s_pulse_timer;
static uint8_t s_history[REC_HISTORY_N];
static uint8_t s_hist_head; // 下写入位（环形）
static bool s_hold_active;
static uint32_t s_hold_start_ms;
static bool s_hold_fired;
static uint32_t s_last_mark_count = UINT32_MAX;

// ---------- 计时 ----------

static void format_time(uint32_t ms, char *out, size_t cap)
{
    uint32_t s = ms / 1000u;
    uint32_t h = s / 3600u;
    uint32_t m = (s / 60u) % 60u;
    uint32_t sec = s % 60u;
    if (h > 0u) {
        snprintf(out, cap, "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32, h, m, sec);
    } else {
        snprintf(out, cap, "%02" PRIu32 ":%02" PRIu32, m, sec);
    }
}

// ---------- 电平历史带 ----------

static void push_history(uint8_t level)
{
    s_history[s_hist_head] = level;
    s_hist_head = (uint8_t)((s_hist_head + 1u) % REC_HISTORY_N);
}

static void bars_render(void)
{
    for (uint8_t i = 0; i < REC_BARS; i++) {
        // 最右 = 最新：偏移 (BARS-1-i) 回找
        uint8_t idx = (uint8_t)((s_hist_head + REC_HISTORY_N - 1u -
                                 (REC_BARS - 1u - i)) % REC_HISTORY_N);
        uint8_t lvl = s_history[idx];
        uint32_t h = 6u + (uint32_t)lvl * (REC_BAR_AREA_H - 6u) / REC_METER_LEVEL_MAX;
        lv_obj_set_height(s_bars[i], (int32_t)h);
    }
}

// ---------- 标记刻度 ----------

static void marks_render(void)
{
    // 清旧刻度（轨道子对象全删）
    uint32_t n = lv_obj_get_child_count(s_track);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_delete(lv_obj_get_child(s_track, 0));
    }
    uint32_t elapsed = buzz_recorder_elapsed_ms();
    uint32_t count = buzz_recorder_mark_count();
    if (elapsed == 0u) {
        return;
    }
    int32_t track_w = lv_obj_get_width(s_track);
    for (uint32_t k = 0; k < count; k++) {
        uint32_t mark = buzz_recorder_mark_ms(k);
        int32_t x = (int32_t)((uint64_t)mark * (uint64_t)track_w / (uint64_t)elapsed);
        lv_obj_t *tick = lv_obj_create(s_track);
        lv_obj_set_size(tick, REC_MARK_TICK_W, REC_MARK_TICK_H);
        lv_obj_align(tick, LV_ALIGN_LEFT_MID, x, 0);
        lv_obj_set_style_bg_color(tick, UI_COLOR_BRAND, 0);
        lv_obj_set_style_radius(tick, 1, 0);
        lv_obj_set_style_border_width(tick, 0, 0);
        lv_obj_set_scrollable(tick, false);
        lv_obj_set_clickable(tick, false);
    }
}

// ---------- 轮询（计时/电平/暂停标签/刻度） ----------

static void rec_poll(lv_timer_t *timer)
{
    (void)timer;
    if (!lv_obj_is_visible(s_screen)) {
        return; // 页面隐藏即停更（timer 只在显示时 resume）
    }
    char buf[16];
    format_time(buzz_recorder_elapsed_ms(), buf, sizeof(buf));
    lv_label_set_text(s_timer_lab, buf);

    buzz_rec_state_t st = buzz_recorder_state();
    // 窗口峰值采样（M5.3 定标）：200ms 窗口取块目标峰值，保留语音包络动态
    uint8_t lvl = (st == BUZZ_REC_STATE_RECORDING) ? buzz_recorder_level_peak() : 0;
    push_history(lvl);
    bars_render();
    lv_label_set_text(s_pause_lab, st == BUZZ_REC_STATE_PAUSED ? "继续" : "暂停");

    uint32_t mc = buzz_recorder_mark_count();
    if (mc != s_last_mark_count) {
        s_last_mark_count = mc;
        marks_render();
    }

    // 无卡红条（录音中拔卡由写错误事件走 toast/退出；此处覆盖开始即无卡与状态翻转）
    static bool s_last_mounted = true;
    bool mounted = buzz_sdcard_mounted();
    if (mounted != s_last_mounted) {
        s_last_mounted = mounted;
        lv_obj_set_hidden(s_no_card_bar, mounted);
    }
}

// REC 点呼吸（§1.4：1200ms 透明度 1→0.25 循环）
static void pulse_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void pulse_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    static bool s_started;
    if (!s_started) {
        s_started = true;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_rec_dot);
        lv_anim_set_values(&a, LV_OPA_COVER, 64);
        lv_anim_set_duration(&a, UI_REC_PULSE_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, pulse_cb);
        lv_anim_start(&a);
    }
}

// ---------- 按住停止（§3 按住确认：800ms 进度填充） ----------

static void hold_reset(void)
{
    s_hold_active = false;
    s_hold_fired = false;
    lv_obj_set_width(s_hold_fill, 0);
}

static void hold_area_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (s_hold_fired) {
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (!s_hold_active) {
            s_hold_active = true;
            s_hold_start_ms = (uint32_t)(lv_tick_get());
        }
        uint32_t held = lv_tick_get() - s_hold_start_ms;
        if (held >= REC_HOLD_STOP_MS) {
            s_hold_fired = true;
            lv_obj_set_width(s_hold_fill, LV_PCT(100));
            (void)ui_rec_request_toggle(); // 录音中 toggle=停止
            return;
        }
        lv_obj_set_width(s_hold_fill, (int32_t)((uint64_t)held * 100u / REC_HOLD_STOP_MS));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        hold_reset(); // 未按满 800ms 松开：进度清零，不触发
    }
}

// ---------- 暂停按钮 ----------

static void pause_btn_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        // 诊断：PRESSED 有日志而 CLICKED 无 = 点击被分类问题吞掉（复测判定用）
        ESP_LOGI("scr_rec", "pause btn PRESSED");
        return;
    }
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI("scr_rec", "pause btn CLICKED → pause_toggle");
        (void)ui_rec_request_pause_toggle();
    }
}

// ---------- 构建 ----------

void scr_rec_create(void)
{
    s_screen = lv_obj_create(NULL);
    ui_theme_apply_screen(s_screen);

    lv_obj_t *page = lv_obj_create(s_screen);
    lv_obj_set_size(page, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_opa(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_hor(page, UI_SAFE_HOR, 0);
    lv_obj_set_style_pad_top(page, UI_SAFE_TOP, 0);
    lv_obj_set_style_pad_bottom(page, UI_PAGE_PAD_BOTTOM, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page, UI_SPACE_4, 0);
    lv_obj_set_scrollable(page, false);

    // 无卡红条（默认隐藏；ux-device §7）
    s_no_card_bar = lv_obj_create(page);
    lv_obj_set_size(s_no_card_bar, LV_PCT(100), 28);
    lv_obj_set_style_bg_color(s_no_card_bar, UI_COLOR_RECORDING, 0);
    lv_obj_set_style_bg_opa(s_no_card_bar, LV_OPA_20, 0);
    lv_obj_set_style_border_color(s_no_card_bar, UI_COLOR_RECORDING, 0);
    lv_obj_set_style_border_width(s_no_card_bar, 1, 0);
    lv_obj_set_style_radius(s_no_card_bar, UI_RADIUS_MD, 0);
    lv_obj_set_scrollable(s_no_card_bar, false);
    lv_obj_set_clickable(s_no_card_bar, false);
    lv_obj_set_hidden(s_no_card_bar, true);
    lv_obj_t *nc = lv_label_create(s_no_card_bar);
    lv_label_set_text(nc, "无存储卡");
    ui_theme_apply_text(nc, FONT_CAPTION, UI_COLOR_RECORDING);
    lv_obj_center(nc);

    // REC 点 + 计时
    lv_obj_t *top = lv_obj_create(page);
    lv_obj_set_size(top, LV_PCT(100), 76);
    lv_obj_set_style_bg_opa(top, 0, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(top, UI_SPACE_2, 0);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(top, false);
    s_rec_dot = lv_obj_create(top);
    lv_obj_set_size(s_rec_dot, 12, 12);
    lv_obj_set_style_radius(s_rec_dot, UI_RADIUS_FULL, 0);
    lv_obj_set_style_bg_color(s_rec_dot, UI_COLOR_RECORDING, 0);
    lv_obj_set_style_border_width(s_rec_dot, 0, 0);
    lv_obj_set_scrollable(s_rec_dot, false);
    lv_obj_set_clickable(s_rec_dot, false);
    s_timer_lab = lv_label_create(top);
    ui_theme_apply_text(s_timer_lab, FONT_TIMER, UI_COLOR_TEXT_PRIMARY);
    lv_label_set_text(s_timer_lab, "00:00");

    // 标记刻度轨道（刻度条上方叠刻度：刻度叠在轨道上，轨道即"计时条"）
    s_track = lv_obj_create(page);
    lv_obj_set_size(s_track, LV_PCT(100), 4);
    lv_obj_set_style_bg_color(s_track, UI_COLOR_CARD2, 0);
    lv_obj_set_style_radius(s_track, 2, 0);
    lv_obj_set_style_border_width(s_track, 0, 0);
    lv_obj_set_style_pad_all(s_track, 0, 0);
    lv_obj_set_scrollable(s_track, false);
    lv_obj_set_clickable(s_track, false);

    // 电平条（历史带）
    lv_obj_t *bars = lv_obj_create(page);
    lv_obj_set_size(bars, LV_PCT(100), REC_BAR_AREA_H);
    lv_obj_set_style_bg_opa(bars, 0, 0);
    lv_obj_set_style_border_width(bars, 0, 0);
    lv_obj_set_style_pad_all(bars, 0, 0);
    lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bars, REC_BAR_GAP, 0);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(bars, false);
    for (uint8_t i = 0; i < REC_BARS; i++) {
        s_bars[i] = lv_obj_create(bars);
        lv_obj_set_size(s_bars[i], REC_BAR_W, 6);
        lv_obj_set_style_bg_color(s_bars[i], UI_COLOR_RECORDING, 0);
        lv_obj_set_style_radius(s_bars[i], 3, 0);
        lv_obj_set_style_border_width(s_bars[i], 0, 0);
        lv_obj_set_style_pad_all(s_bars[i], 0, 0);
        lv_obj_set_scrollable(s_bars[i], false);
        lv_obj_set_clickable(s_bars[i], false);
    }

    // 弹性空隙
    lv_obj_t *spacer = lv_obj_create(page);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, 0, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);

    // 暂停/继续（次按钮：card2 底）
    s_pause_btn = lv_obj_create(page);
    lv_obj_set_size(s_pause_btn, LV_PCT(100), 56);
    lv_obj_set_style_bg_color(s_pause_btn, UI_COLOR_CARD2, 0);
    lv_obj_set_style_radius(s_pause_btn, UI_RADIUS_LG, 0);
    lv_obj_set_style_border_width(s_pause_btn, 0, 0);
    lv_obj_set_style_pad_all(s_pause_btn, 0, 0);
    lv_obj_set_scrollable(s_pause_btn, false);
    lv_obj_set_clickable(s_pause_btn, true);
    s_pause_lab = lv_label_create(s_pause_btn);
    ui_theme_apply_text(s_pause_lab, FONT_BODY, UI_COLOR_TEXT_PRIMARY);
    lv_label_set_text(s_pause_lab, "暂停");
    lv_obj_center(s_pause_lab);
    lv_obj_add_event_cb(s_pause_btn, pause_btn_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_pause_btn, pause_btn_event, LV_EVENT_PRESSED, NULL);

    // 按住停止（§3 按住确认：红边 + 进度填充 800ms）
    s_hold_area = lv_obj_create(page);
    lv_obj_set_size(s_hold_area, LV_PCT(100), 56);
    lv_obj_set_style_bg_opa(s_hold_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_hold_area, UI_COLOR_RECORDING, 0);
    lv_obj_set_style_border_width(s_hold_area, 1, 0);
    lv_obj_set_style_radius(s_hold_area, UI_RADIUS_LG, 0);
    lv_obj_set_style_pad_all(s_hold_area, 0, 0);
    lv_obj_set_scrollable(s_hold_area, false);
    lv_obj_set_clickable(s_hold_area, true);
    s_hold_fill = lv_obj_create(s_hold_area);
    lv_obj_set_size(s_hold_fill, 0, LV_PCT(100));
    lv_obj_align(s_hold_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_hold_fill, UI_COLOR_RECORDING, 0);
    lv_obj_set_style_bg_opa(s_hold_fill, REC_HOLD_FILL_OPA, 0);
    lv_obj_set_style_border_width(s_hold_fill, 0, 0);
    lv_obj_set_style_radius(s_hold_fill, UI_RADIUS_LG, 0);
    lv_obj_set_style_pad_all(s_hold_fill, 0, 0);
    lv_obj_set_scrollable(s_hold_fill, false);
    lv_obj_set_clickable(s_hold_fill, false);
    s_hold_lab = lv_label_create(s_hold_area);
    ui_theme_apply_text(s_hold_lab, FONT_BODY, UI_COLOR_RECORDING);
    lv_label_set_text(s_hold_lab, "按住停止");
    lv_obj_center(s_hold_lab);
    // LVGL 9 add_event_cb 过滤参为单个事件码：按位或会注册到不存在的码上
    // 导致事件永不触发（M5.3 实机按住停止无响应根因）——逐码注册
    lv_obj_add_event_cb(s_hold_area, hold_area_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_hold_area, hold_area_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_hold_area, hold_area_event, LV_EVENT_PRESS_LOST, NULL);

    // 定时器：数据轮询 + 脉冲（创建即暂停，show 时 resume）
    s_poll_timer = lv_timer_create(rec_poll, REC_POLL_MS, NULL);
    lv_timer_pause(s_poll_timer);
    s_pulse_timer = lv_timer_create(pulse_timer_cb, 200, NULL);
    lv_timer_ready(s_pulse_timer);
    lv_timer_pause(s_pulse_timer);
}

void scr_rec_show(bool show)
{
    if (s_screen == NULL) {
        return;
    }
    bsp_display_lock(0);
    if (show) {
        screen_pwr_set_on(true); // 熄屏直接开始录音时先点亮（ux-device §1 含熄屏）
        s_prev_screen = lv_screen_active();
        // 复位会话可视化状态
        memset(s_history, 0, sizeof(s_history));
        s_hist_head = 0;
        s_last_mark_count = UINT32_MAX;
        hold_reset();
        lv_screen_load(s_screen);
        lv_timer_resume(s_poll_timer);
        lv_timer_resume(s_pulse_timer);
    } else {
        lv_timer_pause(s_poll_timer);
        lv_timer_pause(s_pulse_timer);
        if (s_prev_screen != NULL) {
            lv_screen_load(s_prev_screen);
        }
    }
    bsp_display_unlock();
}

void scr_rec_on_rec_event(buzz_rec_evt_t evt)
{
    switch (evt) {
    case BUZZ_REC_EVT_STARTED:
        scr_rec_show(true);
        break;
    case BUZZ_REC_EVT_STOPPED:
    case BUZZ_REC_EVT_AUTO_STOPPED:
    case BUZZ_REC_EVT_ERROR:
        scr_rec_show(false);
        break;
    default:
        break;
    }
}

void scr_rec_set_no_card(bool no_card)
{
    if (s_no_card_bar != NULL) {
        bsp_display_lock(0);
        lv_obj_set_hidden(s_no_card_bar, !no_card);
        bsp_display_unlock();
    }
}
