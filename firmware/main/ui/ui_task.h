// Beecord 手表 UI 任务与页面框架（ui_task.h，M5.1）。
// 职责：bsp_display_start + esp_lvgl_port 托管 LVGL 任务、UI 消息队列骨架、
// 三主页 tileview 横滑 + 上滑设置 + 模态录音页框架 + 页面指示点。
// 线程约定：LVGL 非线程安全，所有 lv_* 调用必须持有 bsp_display_lock/unlock。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "esp_err.h"

// UI 消息来源（M5.1 仅消费 TIMER/DEBUG，其余为 M5.2+ 预留）
typedef enum {
    UI_MSG_SRC_KEY = 0,  // 按键（M5.2 keyd）
    UI_MSG_SRC_REC,      // 录音状态（M5.3）
    UI_MSG_SRC_BLE,      // BLE 连接/同步（M5.4）
    UI_MSG_SRC_PWR,      // 电源（M5.4）
    UI_MSG_SRC_TIMER,    // 定时器
    UI_MSG_SRC_DEBUG,    // REPL 调试
} ui_msg_src_t;

typedef struct {
    ui_msg_src_t src;
    int32_t code;   // 来源内事件码（由各子系统定义）
    int32_t value;  // 事件参数
} ui_msg_t;

// 初始化 display + LVGL + 页面框架（app_main 中 BLE 之后、REPL 之前调用）。
// 失败返回错误码但不致命：REPL 仍可用，ui 命令会报"UI 未初始化"。
esp_err_t ui_init(void);

bool ui_is_ready(void);

// 向 UI 投递消息（任意任务可调用；队列满则丢弃并告警日志）
esp_err_t ui_msg_post(const ui_msg_t *msg);

// 触摸调试：打印 esp_lcd_touch 读取路径的原始触点（REPL `ui touch`）
void ui_debug_touch_dump(void);

// 页面导航 API（供 scr_* / REPL 调试使用）
void ui_nav_to_home(void);   // 回表盘主页（tile 0,0）
// 调试导航（REPL `ui tile`）：跳到任意 tile（col 0..2，row 0 主页行 / 1 设置行）
void ui_nav_to_tile(uint8_t col, uint8_t row);
bool ui_is_main_screen_active(void);

#ifdef __cplusplus
}
#endif
