// Beecord AXP2101 实机电源适配（power_axp.h，M5.4 二阶段）。
// 复用 BSP 共享 I2C handle（bsp_i2c_get_handle，GPIO14/15）——不重建总线，
// 避免影响触控/codec。1Hz 轮询：读电量 → power 纯逻辑平滑/阈值 → UI 消息队列。
// 读取失败降级（跳过本次 + 限频日志），绝不 abort。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

#include "sys/power.h"

// 初始化 I2C 设备 + 创建 1Hz 轮询任务（power 实例模块内静态持有）。
// 须在 bsp i2c 就绪（bsp_i2c_get_handle 非空）与 UI 消息队列创建后调用。
esp_err_t power_axp_start(void);

// REPL 观测：原始电压 mV / 原始百分比 / 平滑百分比 / 充电态 / 读写健康
void power_axp_get_debug(float *vbat_mv, uint8_t *raw_pct, uint8_t *smoothed_pct,
                         bool *charging, uint32_t *read_ok, uint32_t *read_errs);

#ifdef __cplusplus
}
#endif
