// LVGL 快照 → BMP：REPL `ui snap` 写 SD，或 `ui snapusb` 直接 Base64 输出。
// 后者由 LVGL 任务抓帧、REPL 任务输出，未挂载卡也可验收（M5.5）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "esp_err.h"

// 截取当前活跃 screen → 24bit BMP 写 SD 卡根目录 snap.bmp。
// 必须在 LVGL 任务上下文调用（经 ui_snapshot_request_async 派发）。
esp_err_t ui_snapshot_to_sdcard(void);

// 未挂载卡时使用的同一 BMP 快照，直接以 Base64 输出到 USB 控制台。
esp_err_t ui_snapshot_to_console(void);
esp_err_t ui_snapshot_console_emit(void);

// 异步快照：post 到 UI 消息队列，由 LVGL 任务执行；随后用 ui_snapshot_wait_done 取结果。
esp_err_t ui_snapshot_request_async(void);
esp_err_t ui_snapshot_console_request_async(void);

// 等待最近一次异步快照完成，返回其执行结果（timeout_ms 内未完成返回 ESP_ERR_TIMEOUT）。
esp_err_t ui_snapshot_wait_done(uint32_t timeout_ms);

// 供 ui_task 消息泵在 LVGL 任务上下文调用（执行快照并给出完成信号）。
void ui_snapshot_execute_from_ui_task(void);

#ifdef __cplusplus
}
#endif
