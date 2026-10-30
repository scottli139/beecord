// Beecord PCF85063 RTC 实机适配（rtc_pcf.h，M5.4 三阶段）。
// 复用 BSP 共享 I2C（0x51），不重建总线；读取失败返回错误（调用方降级），不 abort。
// OS 时钟完整性标志：振荡器停过/未对时时报告无效——绝不把 2000/1970 垃圾当真时间。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include "esp_err.h"

#include "sys/rtc.h"

// 初始化（加 I2C 设备；幂等）。须在 bsp i2c 就绪后调用。
esp_err_t rtc_pcf_init(void);

// 读本地日历时间。os_stopped=true 表示振荡器曾停止（数据不可信）。
// 读失败返回错误；out 仅在 ESP_OK 时有意义。
esp_err_t rtc_pcf_read(rtc_datetime_t *out, bool *os_stopped);

// 写本地日历时间（写前停振、写完起振并读回校验，清 OS；失败尽力恢复起振）。
esp_err_t rtc_pcf_write(const rtc_datetime_t *local);

// ---------- 表盘时钟服务（M5.4 收尾） ----------
// 1Hz 低频任务读 RTC 维护"最后有效本地时间"快照；scr_home 经 UI 侧定时器拉取
// 渲染（避免 LVGL 任务内做 I2C）。须在 rtc_pcf_init 后调用。
esp_err_t rtc_pcf_clock_service_start(void);

// 取时钟快照（任意任务；瞬态副本）。valid=false = 未对时/振荡器无效/连续失败。
void rtc_pcf_clock_snapshot(rtc_datetime_t *out, bool *valid);

#ifdef __cplusplus
}
#endif
