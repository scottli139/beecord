// Beecord 手表 UI 设计令牌实现（theme.c）。
#include "theme.h"

void ui_theme_apply_screen(lv_obj_t *screen)
{
    lv_obj_set_size(screen, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(screen, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_scrollable(screen, false);
}

void ui_theme_apply_page(lv_obj_t *container)
{
    // LVGL 默认浅色主题会给 tileview/tile/panel 浅灰底、边框、内边距与滚动条，
    // 全部清掉：手表页面一律纯黑底、无装饰（DESIGN_SYSTEM §6 规则 5）
    lv_obj_set_style_bg_color(container, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_shadow_width(container, 0, 0);
    lv_obj_set_style_outline_width(container, 0, 0);
    lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);
}

void ui_theme_apply_text(lv_obj_t *label, const lv_font_t *font, lv_color_t color)
{
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    // 单倍行距 ×1.3 由 LVGL 默认 line_space 提供；手表文字一律单行截断
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}
