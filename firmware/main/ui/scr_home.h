// 表盘主页（scr_home.h）：传统表盘——大时间/日期 + 状态小件 + 提示语 + 录音中横幅。
// M5.4 已接 RTC 与 AXP2101 电量；存储/待同步/最近录音小件和 BLE 状态由 M5.5 接真。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// 在 tileview 主页 tile 内构建表盘
void scr_home_create(lv_obj_t *tile);

// 录音中横幅（含状态栏 REC 点）显隐；M5.3 由录音状态驱动，M5.1 仅接口
void scr_home_show_rec_banner(bool show);

// M5.4：状态栏电量接真（pct 0~100；0xFF=未知态显示"--%"空壳；charging 显闪电）
void scr_home_set_power(uint8_t pct, bool charging);

#ifdef __cplusplus
}
#endif
