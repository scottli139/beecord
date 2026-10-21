// Beecord 手表 UI Toast 实现（ui_toast.c，M5.3）。
#include "ui_toast.h"

#include <stdio.h>
#include <string.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_log.h"
#include "lvgl.h"

#include "screen_pwr.h"
#include "theme.h"

static const char *TAG = "ui_toast";

#define TOAST_TEXT_MAX 48u

static lv_obj_t *s_toast;
static lv_obj_t *s_label;
static lv_timer_t *s_hide_timer;
static char s_text[TOAST_TEXT_MAX];

static void toast_ensure_created(void); // 定义在 ui_toast_init 之后（懒创建）

static void toast_hide(lv_timer_t *timer)
{
    (void)timer;
    lv_obj_set_hidden(s_toast, true);
}

void ui_toast_show(const char *text, bool err)
{
    // 注意：s_toast 为懒创建（首次弹出才建对象），不能作为有效性守卫——
    // 否则首次调用永远提前返回、toast 永不可见（M5.3 实机实锤）。
    // 有效性以 init 完成（s_hide_timer 就绪）为准。
    if (text == NULL || s_hide_timer == NULL) {
        return;
    }
    // 熄屏分级处置（M5.3 实机定案，ux-device §7）：普通提示吞掉防漏光（layer_top
    // 在黑层之上，弹出会漏光）；错误/重要通知先唤醒屏幕再弹——人正看着表等反馈。
    const bool was_off = screen_pwr_is_off();
    ESP_LOGI(TAG, "show: '%s' err=%d screen_off=%d", text, (int)err, (int)was_off);
    if (was_off) {
        if (!err) {
            return;
        }
        screen_pwr_set_on(true); // 唤醒后走正常 10s 自动熄屏（off_monitor）
    }
    snprintf(s_text, sizeof(s_text), "%s", text);
    bsp_display_lock(0);
    toast_ensure_created();
    lv_label_set_text(s_label, s_text);
    lv_obj_set_hidden(s_toast, false);
    // 计时胶囊：1.6s（错误 3s）；重触发重置
    lv_timer_reset(s_hide_timer);
    lv_timer_set_period(s_hide_timer, err ? 3000 : 1600);
    lv_timer_resume(s_hide_timer);
    bsp_display_unlock();
}

void ui_toast_init(void)
{
    // Bug1（M5.3 实机录音页按钮无响应）注记：toast 懒创建——layer_top 在首次
    // 弹 toast 前零对象占用，彻底排除胶层拦截屏幕输入的嫌疑；且创建即
    // 显式清除全部输入相关 flag（非 clickable 对象理论上不拦截命中测试，
    // 这里做防御性收敛）。
    s_hide_timer = lv_timer_create(toast_hide, 1600, NULL);
    lv_timer_pause(s_hide_timer);
    ESP_LOGI(TAG, "toast up (lazy)");
}

// 首次弹 toast 时建对象（LVGL 任务上下文调用）
static void toast_ensure_created(void)
{
    if (s_toast != NULL) {
        return;
    }
    lv_obj_t *top = lv_layer_top();
    s_toast = lv_obj_create(top);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, 32);
    lv_obj_set_style_bg_color(s_toast, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_toast, UI_RADIUS_FULL, 0);
    lv_obj_set_style_border_width(s_toast, 0, 0);
    lv_obj_set_style_pad_hor(s_toast, UI_SPACE_4, 0);
    lv_obj_set_style_pad_ver(s_toast, 0, 0);
    lv_obj_set_scrollable(s_toast, false);
    lv_obj_set_clickable(s_toast, false);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, UI_SAFE_TOP + UI_SPACE_1);

    s_label = lv_label_create(s_toast);
    ui_theme_apply_text(s_label, FONT_CAPTION, UI_COLOR_TEXT_PRIMARY);
    lv_obj_set_hidden(s_toast, true);
}
