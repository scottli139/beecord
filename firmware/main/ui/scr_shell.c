// 空壳页面与页面公共助手实现（scr_shell.c，M5.1）。
#include "scr_shell.h"

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "theme.h"

lv_obj_t *scr_page_begin(lv_obj_t *tile)
{
    // tile 为 tileview 内容容器：透明底露出屏黑，滚动由 tileview 手势接管
    lv_obj_set_style_bg_opa(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_scrollable(tile, false);

    lv_obj_t *page = lv_obj_create(tile);
    lv_obj_set_size(page, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_opa(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    // 页面 padding 16/16/30（§1.3 手表行）+ 上安全区 14
    lv_obj_set_style_pad_hor(page, UI_SAFE_HOR, 0);
    lv_obj_set_style_pad_top(page, UI_SAFE_TOP, 0);
    lv_obj_set_style_pad_bottom(page, UI_PAGE_PAD_BOTTOM, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(page, false);
    return page;
}

void scr_page_title(lv_obj_t *page, const char *title)
{
    // 居中（DESIGN_SYSTEM v1.9 §3）：全宽 + 文本居中，规避 r≈44 圆角弧左上角裁切
    lv_obj_t *lab = lv_label_create(page);
    lv_obj_set_width(lab, LV_PCT(100));
    lv_label_set_text(lab, title);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
    ui_theme_apply_text(lab, FONT_TITLE, UI_COLOR_TEXT_PRIMARY);
}

void scr_list_create(lv_obj_t *tile)
{
    lv_obj_t *page = scr_page_begin(tile);
    scr_page_title(page, "录音列表"); // 内容 M5.5 接
}

void scr_sync_create(lv_obj_t *tile)
{
    lv_obj_t *page = scr_page_begin(tile);
    scr_page_title(page, "同步"); // 内容 M5.5 接
}

void scr_settings_create(lv_obj_t *tile)
{
    lv_obj_t *page = scr_page_begin(tile);
    scr_page_title(page, "设置"); // 内容 M5.5 接
}

// ---------- 模态录音页：M5.3 起由 scr_rec.c 实现 ----------

