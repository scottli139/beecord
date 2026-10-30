// 空壳页面与页面公共助手（scr_shell.h）：录音列表/同步/设置空壳 + 录音模态页框架。
// M5.1 只做标题占位；列表/同步/设置内容由 M5.5 填充，录音页已由 M5.3 落地。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// 页面公共骨架：tile → 透明底 + 页面 padding（上14/左右16/下30）的 flex column 容器，
// 返回容器供各页填充
lv_obj_t *scr_page_begin(lv_obj_t *tile);

// 页面标题（title 15B，text.primary，左对齐于页面 padding 内）
void scr_page_title(lv_obj_t *page, const char *title);

void scr_list_create(lv_obj_t *tile);
void scr_sync_create(lv_obj_t *tile);
void scr_settings_create(lv_obj_t *tile);

// 录音页已由 scr_rec.c 实现（M5.3 接管模态录音页）

#ifdef __cplusplus
}
#endif
