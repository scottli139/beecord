// 熄屏管理器（screen_pwr.h，M5.1）。
// 设计依据：DESIGN_SYSTEM v1.6 §3 熄屏层（z60 纯黑/触控关闭）+ M5_PLAN D6。
// 实现：纯黑覆盖层 + 触摸 indev 停用 + bsp_display_backlight_off() 三件套；
// 无操作计时取 LVGL 内置 indev inactive time；BOOT 键（GPIO0）临时唤醒方案，
// M5.2 由 keyd 按键子系统事件替换（见 screen_pwr.c 注记）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

// 默认 10s；M5.4 启动时由 settings 载入持久值，运行期可经 screen_pwr_set_timeout_ms 修改
#define UI_SCREEN_OFF_MS 10000

// 初始化：创建熄屏黑层（叠加在最上层，z 序=创建顺序最后）+ 无操作监视 + BOOT 唤醒轮询。
// 须在 LVGL 锁内、页面构建完成后调用。
esp_err_t screen_pwr_init(lv_obj_t *screen, lv_indev_t *indev);

// 亮/熄屏（REPL `screen off|on` 与唤醒路径用）；返回当前是否已熄屏
bool screen_pwr_set_on(bool on);
bool screen_pwr_is_off(void);

// M5.4：无操作熄屏超时设置（0=永不熄屏；持久化由 settings 负责，本函数只生效）
void screen_pwr_set_timeout_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
