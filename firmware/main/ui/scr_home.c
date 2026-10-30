// 表盘主页实现（scr_home.c，M5.1 静态版）。
// 布局依据：DESIGN_SYSTEM §3 手表组件 + 图版 ds-watch-components.png ① 屏幕解剖。
// 页面 padding 16/16/30，上安全区 14；所有颜色/间距/圆角取 theme.h 令牌。
#include "scr_home.h"

#include <stdio.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "scr_shell.h"
#include "sys/rtc_pcf.h"
#include "theme.h"

// 状态栏：BLE 空心点 8px + "BLE"，右侧 电量% + 电池壳 20×10 + REC 点（默认隐藏）
static lv_obj_t *s_rec_dot;
static lv_obj_t *s_pct_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_sec_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_batt_fill;
static lv_obj_t *s_bolt_canvas;

// 充电闪电位图（10×14 I1，行起止列；DESIGN §3 状态图标·充电加闪电·的等效小图标）
#define BOLT_W 10u
#define BOLT_H 14u
static uint8_t s_bolt_buf[((BOLT_W + 7u) / 8u) * BOLT_H];
static const uint8_t s_bolt_rows[BOLT_H][2] = {
    { 5, 7 }, { 5, 7 }, { 5, 7 }, { 4, 7 }, { 4, 6 }, { 4, 6 },
    { 3, 5 }, { 3, 5 }, { 2, 4 }, { 2, 4 }, { 3, 4 }, { 3, 4 }, { 0xFF, 0 }, { 0xFF, 0 },
};

static void draw_bolt(void)
{
    lv_color_t on = { .blue = 1 }; // I1：color.blue 为 palette 索引
    for (uint32_t y = 0; y < BOLT_H; y++) {
        const uint8_t x0 = s_bolt_rows[y][0];
        const uint8_t x1 = s_bolt_rows[y][1];
        if (x0 == 0xFFu) {
            continue;
        }
        for (uint32_t x = x0; x <= x1 && x < BOLT_W; x++) {
            lv_canvas_set_px(s_bolt_canvas, (int32_t)x, (int32_t)y, on, LV_OPA_COVER);
        }
    }
}

// M5.4：状态栏电量接真（UI 消息队列/LVGL 上下文调用；pct=0xFF 未知态）。
// 视觉：pct 文本 + 填充宽 = 百分比；<15% 橙（§3）；充电态显琥珀闪电。
void scr_home_set_power(uint8_t pct, bool charging)
{
    bsp_display_lock(0);
    if (pct == 0xFFu) {
        lv_label_set_text(s_pct_label, "--%");
        lv_obj_set_width(s_batt_fill, 0);
        lv_obj_set_hidden(s_bolt_canvas, true);
        bsp_display_unlock();
        return;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
    lv_label_set_text(s_pct_label, buf);
    const int32_t fill_w = (int32_t)((UI_BATTERY_W - 2) * pct / 100u);
    lv_obj_set_width(s_batt_fill, fill_w);
    lv_obj_set_style_bg_color(s_batt_fill,
                              pct < 15u ? UI_COLOR_WARNING : UI_COLOR_SUCCESS, 0);
    lv_obj_set_hidden(s_bolt_canvas, !charging);
    bsp_display_unlock();
}

static void status_bar_create(lv_obj_t *page)
{
    lv_obj_t *bar = lv_obj_create(page);
    lv_obj_set_size(bar, LV_PCT(100), UI_STATUS_BAR_H);
    lv_obj_set_style_bg_opa(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    // 贴顶行的四角圆角补偿：页面安全区（上 24/左右 20，M5.1 实机修订）之外再内缩 8 → 28
    lv_obj_set_style_pad_hor(bar, UI_SPACE_2, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(bar, false);

    // 左：BLE 状态（M5.5 接连接态；当前静态空心=未连接）
    lv_obj_t *ble = lv_obj_create(bar);
    lv_obj_set_size(ble, 48, UI_STATUS_BAR_H); // 触控目标 ≥48
    lv_obj_set_style_bg_opa(ble, 0, 0);
    lv_obj_set_style_border_width(ble, 0, 0);
    lv_obj_set_style_pad_all(ble, 0, 0);
    lv_obj_set_flex_flow(ble, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ble, UI_SPACE_1, 0);
    lv_obj_set_style_pad_left(ble, 0, 0);
    lv_obj_set_flex_align(ble, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(ble, false);
    lv_obj_t *dot = lv_obj_create(ble);
    lv_obj_set_size(dot, UI_STATUS_DOT, UI_STATUS_DOT);
    lv_obj_set_style_radius(dot, UI_RADIUS_FULL, 0);
    lv_obj_set_style_bg_opa(dot, 0, 0); // 空心：仅描边
    lv_obj_set_style_border_color(dot, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_border_width(dot, 1, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_t *ble_txt = lv_label_create(ble);
    lv_label_set_text(ble_txt, "BLE");
    ui_theme_apply_text(ble_txt, FONT_CAPTION, UI_COLOR_TEXT_SECONDARY);

    // 右：电量（M5.4 接 AXP2101：未知 "--%"+空壳，不伪造值）+ 电池壳 + 充电闪电 + REC 点
    lv_obj_t *pwr = lv_obj_create(bar);
    lv_obj_set_size(pwr, 72, UI_STATUS_BAR_H);
    lv_obj_set_style_bg_opa(pwr, 0, 0);
    lv_obj_set_style_border_width(pwr, 0, 0);
    lv_obj_set_style_pad_all(pwr, 0, 0);
    lv_obj_set_flex_flow(pwr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pwr, UI_SPACE_1, 0);
    lv_obj_set_flex_align(pwr, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(pwr, false);
    s_pct_label = lv_label_create(pwr);
    lv_label_set_text(s_pct_label, "--%"); // 未读到有效数据的未知态
    ui_theme_apply_text(s_pct_label, FONT_CAPTION, UI_COLOR_TEXT_SECONDARY);

    // 充电闪电（§3：充电加闪电；2px 圆头线性小图标等效——I1 canvas 位图）
    s_bolt_canvas = lv_canvas_create(pwr);
    lv_obj_set_size(s_bolt_canvas, BOLT_W, BOLT_H);
    lv_obj_set_style_pad_all(s_bolt_canvas, 0, 0);
    lv_obj_set_scrollable(s_bolt_canvas, false);
    lv_obj_set_clickable(s_bolt_canvas, false);
    lv_canvas_set_buffer(s_bolt_canvas, s_bolt_buf, BOLT_W, BOLT_H,
                         LV_COLOR_FORMAT_I1);
    // palette[0]=透明底 [1]=琥珀；set_px 对 I1 取 color.blue 作索引（LVGL 9 实现）
    lv_canvas_set_palette(s_bolt_canvas, 0, (lv_color32_t){ .alpha = 0 });
    lv_canvas_set_palette(s_bolt_canvas, 1,
                          (lv_color32_t){ .red = 0xF5, .green = 0xA6, .blue = 0x23,
                                          .alpha = 0xFF });
    draw_bolt();
    lv_obj_set_hidden(s_bolt_canvas, true);

    lv_obj_t *batt = lv_obj_create(pwr);
    lv_obj_set_size(batt, UI_BATTERY_W, UI_BATTERY_H);
    lv_obj_set_style_radius(batt, 2, 0); // §3 电池壳 20×10 图版实测圆角（≈2px）
    lv_obj_set_style_bg_opa(batt, 0, 0);
    lv_obj_set_style_border_color(batt, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_border_width(batt, 1, 0);
    lv_obj_set_style_pad_all(batt, 0, 0);
    lv_obj_set_scrollable(batt, false);
    s_batt_fill = lv_obj_create(batt);
    lv_obj_set_size(s_batt_fill, 0, UI_BATTERY_H - 2); // 未知态：空壳无填充
    lv_obj_align(s_batt_fill, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_set_style_radius(s_batt_fill, 1, 0);
    lv_obj_set_style_bg_color(s_batt_fill, UI_COLOR_SUCCESS, 0);
    lv_obj_set_style_border_width(s_batt_fill, 0, 0);
    lv_obj_set_style_pad_all(s_batt_fill, 0, 0);
    lv_obj_set_scrollable(s_batt_fill, false);
    s_rec_dot = lv_obj_create(pwr);
    lv_obj_set_size(s_rec_dot, UI_STATUS_DOT, UI_STATUS_DOT);
    lv_obj_set_style_radius(s_rec_dot, UI_RADIUS_FULL, 0);
    lv_obj_set_style_bg_color(s_rec_dot, UI_COLOR_RECORDING, 0);
    lv_obj_set_style_border_width(s_rec_dot, 0, 0);
    lv_obj_set_style_pad_all(s_rec_dot, 0, 0);
    lv_obj_set_hidden(s_rec_dot, true);
}

// 录音中横幅：h36 r12（§3 组件规格）红边 1px + recording 深红底；点击进入录音页（M5.3 接）
static lv_obj_t *s_banner;
static lv_obj_t *s_banner_dot;
static bool s_pulse_started;

static void banner_pulse_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void banner_create(lv_obj_t *page)
{
    s_banner = lv_obj_create(page);
    lv_obj_set_size(s_banner, LV_PCT(100), UI_REC_BANNER_H);
    lv_obj_set_style_bg_color(s_banner, UI_COLOR_REC_BANNER_BG, 0);
    lv_obj_set_style_bg_opa(s_banner, UI_OPA_REC_BANNER_BG, 0);
    lv_obj_set_style_border_color(s_banner, UI_COLOR_RECORDING, 0);
    lv_obj_set_style_border_width(s_banner, 1, 0);
    lv_obj_set_style_border_opa(s_banner, UI_OPA_REC_BANNER_BORDER, 0);
    lv_obj_set_style_radius(s_banner, 12, 0); // §3 横幅规格 r12（§1.3 刻度外，组件行优先）
    lv_obj_set_style_pad_all(s_banner, 0, 0);
    lv_obj_set_style_pad_left(s_banner, UI_SPACE_3, 0);
    lv_obj_set_style_pad_right(s_banner, UI_SPACE_3, 0);
    lv_obj_set_flex_flow(s_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_banner, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_banner, UI_SPACE_2, 0);
    lv_obj_set_scrollable(s_banner, false);
    lv_obj_set_clickable(s_banner, true);

    lv_obj_t *left = lv_obj_create(s_banner);
    lv_obj_set_style_bg_opa(left, 0, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(left, UI_SPACE_2, 0);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(left, false);
    s_banner_dot = lv_obj_create(left);
    lv_obj_set_size(s_banner_dot, UI_STATUS_DOT, UI_STATUS_DOT);
    lv_obj_set_style_radius(s_banner_dot, UI_RADIUS_FULL, 0);
    lv_obj_set_style_bg_color(s_banner_dot, UI_COLOR_RECORDING, 0);
    lv_obj_set_style_border_width(s_banner_dot, 0, 0);
    lv_obj_set_style_pad_all(s_banner_dot, 0, 0);
    lv_obj_t *txt = lv_label_create(left);
    lv_label_set_text(txt, "录音中 00:12:34"); // 计时 M5.3 接
    ui_theme_apply_text(txt, FONT_CAPTION, UI_COLOR_TEXT_PRIMARY);

    lv_obj_t *go = lv_label_create(s_banner);
    lv_label_set_text(go, "查看 ›");
    ui_theme_apply_text(go, FONT_CAPTION, UI_COLOR_TEXT_SECONDARY);

    lv_obj_set_hidden(s_banner, true);
}

void scr_home_show_rec_banner(bool show)
{
    if (s_banner == NULL) {
        return;
    }
    bsp_display_lock(0);
    if (show) {
        lv_obj_set_hidden(s_banner, false);
        lv_obj_set_hidden(s_rec_dot, false);
        // REC 点 pulse：1200ms 透明度 255→64 循环（§1.4）
        if (!s_pulse_started) {
            s_pulse_started = true;
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_banner_dot);
            lv_anim_set_values(&a, LV_OPA_COVER, 64);
            lv_anim_set_duration(&a, UI_REC_PULSE_MS);
            lv_anim_set_repeat_delay(&a, 0);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_exec_cb(&a, banner_pulse_cb);
            lv_anim_start(&a);
        }
    } else {
        lv_obj_set_hidden(s_banner, true);
        lv_obj_set_hidden(s_rec_dot, true);
    }
    bsp_display_unlock();
}

// 状态小件：3 等分卡 r14，标签 10 + 值 15B（设计 16/600，用 Bold 15 代）
static void widget_row_create(lv_obj_t *page)
{
    static const char *labels[3] = {"存储", "待同步", "最近录音"};
    static const char *values[3] = {"62%", "2", "01:36"}; // M5.4/M5.5 接真

    lv_obj_t *row = lv_obj_create(page);
    lv_obj_set_size(row, LV_PCT(100), UI_WIDGET_H);
    lv_obj_set_style_margin_top(row, UI_SPACE_6, 0); // 紧跟时间区（§3 解剖小件 y238 起）
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, UI_SPACE_2, 0);
    lv_obj_set_scrollable(row, false);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *card = lv_obj_create(row);
        // 宽 0 + flex_grow 均分剩余宽：LV_PCT(33) 会算成 122×3+间隙 16=382>370 溢出 12px，
        // 右卡右缘被行边界硬裁（实机快照证实右圆角消失）
        lv_obj_set_size(card, 0, UI_WIDGET_H);
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_style_bg_color(card, UI_COLOR_CARD, 0);
        lv_obj_set_style_radius(card, UI_RADIUS_LG, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_pad_top(card, UI_SPACE_1, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(card, 2, 0);
        lv_obj_set_scrollable(card, false);
        lv_obj_t *lab = lv_label_create(card);
        lv_label_set_text(lab, labels[i]);
        ui_theme_apply_text(lab, FONT_TINY, UI_COLOR_TEXT_SECONDARY);
        lv_obj_t *val = lv_label_create(card);
        lv_label_set_text(val, values[i]);
        ui_theme_apply_text(val, FONT_TITLE, UI_COLOR_TEXT_PRIMARY);
    }
}

// 1Hz 时钟渲染（LVGL 任务上下文）：拉 RTC 服务快照（低频任务维护，不在此做 I2C）
static const char *s_weekday_cn[] = { "周日", "周一", "周二", "周三",
                                      "周四", "周五", "周六" };

static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    rtc_datetime_t dt;
    bool valid = false;
    rtc_pcf_clock_snapshot(&dt, &valid);
    if (!valid) {
        lv_label_set_text(s_time_label, "--:--");
        lv_label_set_text(s_sec_label, "--");
        lv_label_set_text(s_date_label, "未对时");
        return;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)dt.hour, (unsigned)dt.minute);
    lv_label_set_text(s_time_label, buf);
    snprintf(buf, sizeof(buf), ":%02u", (unsigned)dt.second);
    lv_label_set_text(s_sec_label, buf);
    snprintf(buf, sizeof(buf), "%u月%u日 %s", (unsigned)dt.month, (unsigned)dt.day,
             s_weekday_cn[rtc_weekday(&dt)]);
    lv_label_set_text(s_date_label, buf);
}

void scr_home_create(lv_obj_t *tile)
{
    lv_obj_t *page = scr_page_begin(tile);

    // flex 列布局：主轴自上而下，交叉轴（水平）居中——大时间/日期/提示语的设计居中位
    // 依赖于此（lv_obj_align 在 flex 容器内无效）；100% 宽的行（状态栏/小件行/横幅）不受影响
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);

    status_bar_create(page);

    // 大时间（M5.4 收尾接真：display64 HH:MM + 基线右侧小号秒 :SS；未对时 --:-- --）。
    // 秒对齐：行容器底对齐 + 秒标签底部内缩（display 数字基线以下的下沉量）。
    lv_obj_t *time_row = lv_obj_create(page);
    lv_obj_set_size(time_row, LV_SIZE_CONTENT, 84);
    lv_obj_set_style_bg_opa(time_row, 0, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(time_row, 2, 0);
    lv_obj_set_scrollable(time_row, false);
    lv_obj_align(time_row, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_margin_top(time_row, UI_SPACE_8, 0);

    s_time_label = lv_label_create(time_row);
    ui_theme_apply_text(s_time_label, FONT_DISPLAY, UI_COLOR_TEXT_PRIMARY);
    lv_label_set_text(s_time_label, "--:--");

    s_sec_label = lv_label_create(time_row);
    ui_theme_apply_text(s_sec_label, FONT_BODY, UI_COLOR_TEXT_SECONDARY);
    lv_label_set_text(s_sec_label, "--");
    lv_obj_set_style_margin_bottom(s_sec_label, 8, 0); // 秒贴 display 基线

    s_date_label = lv_label_create(page);
    ui_theme_apply_text(s_date_label, FONT_BODY, UI_COLOR_TEXT_SECONDARY);
    lv_label_set_text(s_date_label, "未对时");
    lv_obj_set_style_text_align(s_date_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_date_label, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_margin_top(s_date_label, UI_SPACE_2, 0);

    banner_create(page);

    // 状态小件紧跟时间区（§3 解剖：小件 y238 起，不沉底）
    widget_row_create(page);

    // 弹性空隙把提示语压到底部安全区上方
    lv_obj_t *spacer = lv_obj_create(page);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, 0, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_set_scrollable(spacer, false);

    lv_obj_t *hint = lv_label_create(page);
    lv_label_set_text(hint, "按功能键开始录音");
    // 实机可读性修订：tiny 10 在 410×502 AMOLED 上偏小，提示语提级 caption 11（DESIGN_SYSTEM §1.2 注记）
    ui_theme_apply_text(hint, FONT_CAPTION, UI_COLOR_TEXT_TERTIARY);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(hint, UI_SPACE_4, 0);
    lv_obj_set_style_margin_bottom(hint, UI_SPACE_1, 0);

    lv_timer_create(clock_timer_cb, 1000, NULL); // 表盘时钟 1Hz 渲染
}
