// 按键 GPIO 实机绑定（keyd_gpio.h，M5.2）。
// BOOT=GPIO0（低=按下）；PWR=GPIO10（高=按下，实机已验证；备选扩展器路径
// 经 i2cscan 全扫无未知地址排除，见 HARDWARE.md）。
// 10ms 轮询喂 keyd 状态机，事件投递 UI 消息队列（LVGL 任务上下文分发）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

// 初始化按键轮询任务（BOOT + PWR 双实例）。须在 ui_init 之后调用（依赖消息队列）。
esp_err_t keyd_gpio_init(void);

// REPL 观测：打印 BOOT/PWR 当前原始电平（PWR 验证用）
void keyd_gpio_dump_levels(void);

#ifdef __cplusplus
}
#endif
