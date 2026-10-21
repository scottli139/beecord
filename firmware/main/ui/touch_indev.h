// 原生 LVGL 触摸 indev（touch_indev.h，M5.2 缺陷修复）。
// 为何绕开 esp_lvgl_port 触摸胶层（lvgl_port_add_touch）：
// 该胶层的读回调 lvgl_port_touchpad_read 对 esp_lcd_touch_read_data /
// esp_lcd_touch_get_data 做 ESP_ERROR_CHECK——共享 I2C 总线上偶发的
// ESP_ERR_INVALID_STATE（FT5x06 无触点 NACK / 总线竞争）会被升级成 abort()
// 整机重启（M5.2 实机验收两次实锤）。本模块对读错误降级处理：
// 保留上一次坐标与状态（LVGL indev 契约允许），错误计数每 64 次打一条警告。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_lcd_touch.h"
#include "lvgl.h"

// 创建 POINTER 型 indev 并绑定显示。返回 NULL 失败。
lv_indev_t *ui_touch_indev_create(lv_display_t *disp, esp_lcd_touch_handle_t tp);

// 错误计数观测（REPL `ui touch` 打印）：读回调总次数 / 失败次数
void ui_touch_indev_stats(uint32_t *out_total, uint32_t *out_err);

#ifdef __cplusplus
}
#endif
