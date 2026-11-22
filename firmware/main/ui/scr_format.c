#include "scr_format.h"

#include "buzz_cmds.h"
#include "screen_pwr.h"
#include "theme.h"
#include "ui_toast.h"

static lv_obj_t *s_overlay;
static lv_obj_t *s_title;
static lv_obj_t *s_description;
static lv_obj_t *s_buttons;
static lv_obj_t *s_cancel_button;
static lv_obj_t *s_confirm_button;
static lv_obj_t *s_confirm_label;
static bool s_dismissed;

static void cancel_clicked(lv_event_t *event)
{
    (void)event;
    s_dismissed = true;
    lv_obj_set_hidden(s_overlay, true);
    if (buzz_cmds_format_action(BUZZ_FORMAT_CANCEL) != ESP_OK) {
        ui_toast_show("操作繁忙，请重试", true);
    }
}

static void confirm_clicked(lv_event_t *event)
{
    (void)event;
    if (buzz_cmds_format_action(BUZZ_FORMAT_CONFIRM) != ESP_OK) {
        ui_toast_show("操作繁忙，请重试", true);
    }
}

static lv_obj_t *button_create(lv_obj_t *parent, const char *text, bool danger,
                               lv_event_cb_t callback)
{
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_set_size(button, 0, UI_TOUCH_MIN);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_radius(button, UI_RADIUS_MD, 0);
    lv_obj_set_style_bg_color(button, danger ? UI_COLOR_DANGER : UI_COLOR_CARD2, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_scrollable(button, false);
    lv_obj_set_clickable(button, true);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    ui_theme_apply_text(label, FONT_BODY, UI_COLOR_TEXT_PRIMARY);
    lv_obj_center(label);
    return label;
}

void scr_format_create(void)
{
    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_overlay, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(s_overlay, UI_COLOR_SCRIM, 0);
    lv_obj_set_style_bg_opa(s_overlay, UI_OPA_SCRIM, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_set_clickable(s_overlay, true);

    lv_obj_t *card = lv_obj_create(s_overlay);
    lv_obj_set_size(card, 240, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, UI_COLOR_CARD, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_style_pad_row(card, UI_SPACE_3, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(card, false);

    lv_obj_t *icon = lv_obj_create(card);
    lv_obj_set_size(icon, 28, 28);
    lv_obj_set_style_radius(icon, UI_RADIUS_FULL, 0);
    lv_obj_set_style_bg_color(icon, UI_COLOR_DANGER, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_set_scrollable(icon, false);
    lv_obj_t *mark = lv_label_create(icon);
    lv_label_set_text(mark, "!");
    ui_theme_apply_text(mark, FONT_BODY, UI_COLOR_TEXT_PRIMARY);
    lv_obj_center(mark);

    s_title = lv_label_create(card);
    ui_theme_apply_text(s_title, FONT_TITLE, UI_COLOR_TEXT_PRIMARY);
    lv_obj_set_width(s_title, LV_PCT(100));
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);

    s_description = lv_label_create(card);
    ui_theme_apply_text(s_description, FONT_BODY, UI_COLOR_TEXT_SECONDARY);
    lv_obj_set_width(s_description, LV_PCT(100));
    lv_label_set_long_mode(s_description, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_description, LV_TEXT_ALIGN_CENTER, 0);

    s_buttons = lv_obj_create(card);
    lv_obj_set_size(s_buttons, LV_PCT(100), UI_TOUCH_MIN);
    lv_obj_set_style_bg_opa(s_buttons, 0, 0);
    lv_obj_set_style_border_width(s_buttons, 0, 0);
    lv_obj_set_style_pad_all(s_buttons, 0, 0);
    lv_obj_set_style_pad_column(s_buttons, UI_SPACE_2, 0);
    lv_obj_set_flex_flow(s_buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(s_buttons, false);
    s_cancel_button = lv_obj_get_parent(button_create(s_buttons, "取消", false, cancel_clicked));
    s_confirm_label = button_create(s_buttons, "格式化", true, confirm_clicked);
    s_confirm_button = lv_obj_get_parent(s_confirm_label);
    lv_obj_set_hidden(s_overlay, true);
}

void scr_format_debug_click(bool confirm)
{
    lv_obj_t *button = confirm ? s_confirm_button : s_cancel_button;
    if (button != NULL) {
        lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    }
}

static void show_overlay(void)
{
    if (lv_obj_is_hidden(s_overlay)) {
        screen_pwr_set_on(true);
        lv_obj_set_hidden(s_overlay, false);
        lv_obj_move_foreground(s_overlay);
    }
}

void scr_format_render(const device_snapshot_t *snapshot)
{
    const device_storage_state_t state = snapshot->storage_state;
    if (state == DEVICE_STORAGE_MOUNTED || state == DEVICE_STORAGE_UNAVAILABLE ||
        state == DEVICE_STORAGE_ERROR || state == DEVICE_STORAGE_UNKNOWN) {
        lv_obj_set_hidden(s_overlay, true);
        s_dismissed = false;
        return;
    }
    if (state == DEVICE_STORAGE_FORMAT_CANDIDATE && s_dismissed) {
        return;
    }
    show_overlay();
    lv_obj_set_hidden(s_buttons, state == DEVICE_STORAGE_FORMATTING);
    if (state == DEVICE_STORAGE_FORMATTING) {
        lv_label_set_text(s_title, "格式化中…");
        lv_label_set_text(s_description, "请勿断电");
    } else if (state == DEVICE_STORAGE_FORMAT_FAILED) {
        lv_label_set_text(s_title, "格式化失败");
        lv_label_set_text(s_description, "请重试，卡上数据可能不可用");
        lv_label_set_text(s_confirm_label, "重试");
    } else {
        lv_label_set_text(s_title, "无法识别存储卡");
        lv_label_set_text(s_description, "格式化为 FAT32？卡上全部数据将被清除");
        lv_label_set_text(s_confirm_label, "格式化");
    }
}
