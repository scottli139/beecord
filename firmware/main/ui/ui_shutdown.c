// Beecord 关机警告层实现（ui_shutdown.c，M5.4 二阶段）。
#include "ui_shutdown.h"

#include <stdio.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_log.h"
#include "lvgl.h"

#include "screen_pwr.h"
#include "theme.h"

static const char *TAG = "shutdown";

// PWR 长按 4.5s 触发 → 硬件 ~6s 断电：显示窗口 2.5s（剩余秒数 2→1 倒数）；
// 6s 内未断电（松手）则到时自消退（keyd 无释放通知的文档化短超时方案）
#define WARN_SHOW_MS 2500u
#define WARN_COUNT_STEP_MS 900u // 每 0.9s 减 1 秒

static lv_obj_t *s_layer;
static lv_obj_t *s_title;
static lv_obj_t *s_count;
static lv_timer_t *s_hide_timer;
static lv_timer_t *s_count_timer;
static int s_remaining_s;

static void hide_now(void)
{
    if (s_layer != NULL) {
        lv_obj_set_hidden(s_layer, true);
    }
    lv_timer_pause(s_hide_timer);
    lv_timer_pause(s_count_timer);
}

static void hide_cb(lv_timer_t *timer)
{
    (void)timer;
    hide_now();
}

static void count_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_remaining_s > 1) {
        s_remaining_s--;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", s_remaining_s);
    lv_label_set_text(s_count, buf);
}

void ui_shutdown_warn_show(void)
{
    bsp_display_lock(0);
    if (s_layer == NULL) {
        // 挂 layer_top（高于熄屏黑层/页面；z80 语义）：全屏纯黑不可交互
        lv_obj_t *top = lv_layer_top();
        s_layer = lv_obj_create(top);
        lv_obj_set_size(s_layer, UI_SCREEN_W, UI_SCREEN_H);
        lv_obj_set_style_bg_color(s_layer, UI_COLOR_BG, 0);
        lv_obj_set_style_bg_opa(s_layer, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_layer, 0, 0);
        lv_obj_set_style_radius(s_layer, 0, 0);
        lv_obj_set_style_pad_all(s_layer, 0, 0);
        lv_obj_set_scrollable(s_layer, false);
        lv_obj_set_clickable(s_layer, false);

        lv_obj_t *box = lv_obj_create(s_layer);
        lv_obj_set_size(box, UI_SCREEN_W, 120);
        lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_opa(box, 0, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(box, UI_SPACE_3, 0);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(box, false);
        lv_obj_set_clickable(box, false);

        s_title = lv_label_create(box);
        ui_theme_apply_text(s_title, FONT_BODY, UI_COLOR_TEXT_PRIMARY);
        lv_label_set_text(s_title, "即将关机…");

        s_count = lv_label_create(box);
        ui_theme_apply_text(s_count, FONT_TIMER, UI_COLOR_TEXT_PRIMARY);
        lv_label_set_text(s_count, "2");

        s_hide_timer = lv_timer_create(hide_cb, WARN_SHOW_MS, NULL);
        s_count_timer = lv_timer_create(count_cb, WARN_COUNT_STEP_MS, NULL);
        ESP_LOGI(TAG, "shutdown warn layer up");
    }
    screen_pwr_set_on(true); // 熄屏也要可见（点亮；6s 真断电则无所谓）
    s_remaining_s = 2;
    lv_label_set_text(s_count, "2");
    lv_obj_set_hidden(s_layer, false);
    lv_timer_reset(s_hide_timer);
    lv_timer_set_period(s_hide_timer, WARN_SHOW_MS);
    lv_timer_resume(s_hide_timer);
    lv_timer_reset(s_count_timer);
    lv_timer_resume(s_count_timer);
    bsp_display_unlock();
}
