// 表盘主页（scr_home.h）：传统表盘——大时间/日期 + 状态小件 + 提示语 + 录音中横幅。
// 数据接真在 M5.4/M5.5（RTC/存储/BLE），M5.1 为静态占位（任务书指定 14:05/10月20日 周二/68%）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// 在 tileview 主页 tile 内构建表盘
void scr_home_create(lv_obj_t *tile);

// 录音中横幅（含状态栏 REC 点）显隐；M5.3 由录音状态驱动，M5.1 仅接口
void scr_home_show_rec_banner(bool show);

#ifdef __cplusplus
}
#endif
