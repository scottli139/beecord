// Beecord 手表 UI 设计令牌（theme.h）。
// 唯一取色处：所有 scr_*/ui_* 的颜色、间距、圆角、动效一律引用本文件常量，
// 禁止散落 lv_color_hex(0x...) 字面量（DESIGN_SYSTEM §6 防跑偏规则 1/2/3/7）。
// 令牌来源：beecord/docs/DESIGN_SYSTEM.md §1（手表列）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// ---------- 屏幕模型（410×502 AMOLED，圆角 r≈44） ----------
#define UI_SCREEN_W 410
#define UI_SCREEN_H 502

// ---------- 色彩（DESIGN_SYSTEM §1.1 手表列） ----------
#define UI_COLOR_BRAND lv_color_hex(0xF5A623)        // brand.primary：品牌/主操作
#define UI_COLOR_RECORDING lv_color_hex(0xE5484D)    // state.recording：录音态/REC
#define UI_COLOR_SYNC lv_color_hex(0x0A84FF)         // state.sync：连接/同步
#define UI_COLOR_SUCCESS lv_color_hex(0x30D158)      // state.success：校验通过/完成
#define UI_COLOR_WARNING lv_color_hex(0xFF9F0A)      // state.warning：低电/告警
#define UI_COLOR_DANGER lv_color_hex(0xE5484D)       // state.danger：删除/错误（手表同 recording）
#define UI_COLOR_BG lv_color_hex(0x000000)           // surface.bg：纯黑省电
#define UI_COLOR_CARD lv_color_hex(0x1C1C1E)         // surface.card：卡片
#define UI_COLOR_CARD2 lv_color_hex(0x2C2C2E)        // surface.card2：次级填充/轨道
#define UI_COLOR_TEXT_PRIMARY lv_color_hex(0xECECF1) // text.primary：正文
#define UI_COLOR_TEXT_SECONDARY lv_color_hex(0x9A9AA2) // text.secondary：辅助
#define UI_COLOR_TEXT_TERTIARY lv_color_hex(0x6E6E73)  // text.tertiary：弱提示
// scrim.overlay rgba(0,0,0,.66)：遮罩用"黑底 + 透明度"两段式表达
#define UI_COLOR_SCRIM lv_color_hex(0x000000)
#define UI_OPA_SCRIM LV_OPA_66

// 录音中横幅：红边 1px + state.recording 深红底（低不透明度的红，AMOLED 上近似深红）
#define UI_COLOR_REC_BANNER_BG UI_COLOR_RECORDING
#define UI_OPA_REC_BANNER_BG LV_OPA_20 // §3 只定"深红底"，取 20% 红叠黑底
#define UI_OPA_REC_BANNER_BORDER LV_OPA_100

// ---------- 间距刻度（§1.3，只允许 4 的倍数） ----------
#define UI_SPACE_1 4
#define UI_SPACE_2 8
#define UI_SPACE_3 12
#define UI_SPACE_4 16
#define UI_SPACE_5 20
#define UI_SPACE_6 24
#define UI_SPACE_8 32

// ---------- 安全区与页面 padding ----------
// M5.1 实机多轮实测修订：r≈44 圆角弧 + 表壳边框对四角裁切远超理论值——
// 上 14→24（状态栏/页面标题避顶弧）、左右 16→20（DESIGN_SYSTEM §1.3 已注记）
#define UI_SAFE_TOP 24   // 上安全区
#define UI_SAFE_HOR 20   // 左右安全区 = 页面 padding
#define UI_SAFE_BOTTOM 26 // 下安全区（留给页面指示点）
#define UI_PAGE_PAD_BOTTOM 30 // 页面 padding 16/16/30（DESIGN_SYSTEM §1.3 手表行）

// ---------- 圆角（§1.3） ----------
#define UI_RADIUS_SM 6   // Mac 控件（手表少用）
#define UI_RADIUS_MD 10  // Mac 卡片 / 手表行
#define UI_RADIUS_LG 14  // 手表卡片
#define UI_RADIUS_XL 20  // 浮层卡片
#define UI_RADIUS_FULL LV_RADIUS_CIRCLE

// ---------- 动效（§1.4） ----------
#define UI_MOTION_MICRO 100
#define UI_MOTION_FAST 150
#define UI_MOTION_NORMAL 250
#define UI_MOTION_SLOW 400
#define UI_MOTION_PAGE_SWIPE 200 // 手表页面横滑专用
#define UI_REC_PULSE_MS 1200     // REC 点 pulse 循环（透明度 1→0.25）

// ---------- 字体（§1.2 手表列；生成物见 main/ui/fonts/ 与 tools/font_gen/） ----------
extern const lv_font_t ui_font_cjk10;  // 思源黑体 CN 10/400（tiny，仅提示语与小件标签）
extern const lv_font_t ui_font_cjk11;  // 思源黑体 CN 11/400（caption，辅助说明/状态栏）
extern const lv_font_t ui_font_cjk13;  // 思源黑体 CN 13/400（body）
extern const lv_font_t ui_font_cjk15b; // 思源黑体 CN 15/700（title；代设计 600 档）
extern const lv_font_t ui_font_num56;  // Montserrat Light 56（timer）
extern const lv_font_t ui_font_num64;  // Montserrat Light 64（display）

#define FONT_TINY (&ui_font_cjk10)
#define FONT_CAPTION (&ui_font_cjk11)
#define FONT_BODY (&ui_font_cjk13)
#define FONT_TITLE (&ui_font_cjk15b)
#define FONT_TIMER (&ui_font_num56)
#define FONT_DISPLAY (&ui_font_num64)

// ---------- 组件度量（DESIGN_SYSTEM §3 手表组件规范） ----------
#define UI_STATUS_BAR_H 28        // 状态栏
#define UI_STATUS_DOT 8           // BLE 点 / REC 点
#define UI_BATTERY_W 20           // 电池壳
#define UI_BATTERY_H 10
#define UI_REC_BANNER_H 36        // 录音中横幅
#define UI_PAGE_DOT 6             // 页面指示点
#define UI_PAGE_DOT_GAP 6
#define UI_WIDGET_H 60            // 状态小件卡（3 等分）
#define UI_TOUCH_MIN 48           // 触控目标最小边

// 常用文字样式速设（须在持有 LVGL 锁时调用）
void ui_theme_apply_screen(lv_obj_t *screen);           // 纯黑背景
void ui_theme_apply_page(lv_obj_t *container);          // 页面容器规范化：黑底/无边/零 pad/关滚动条
void ui_theme_apply_text(lv_obj_t *label, const lv_font_t *font,
                         lv_color_t color);               // 文字基础样式

#ifdef __cplusplus
}
#endif
