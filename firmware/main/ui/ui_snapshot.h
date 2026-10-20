// LVGL 快照 → BMP 存 SD（ui_snapshot.h）：REPL `ui snap` 触发，
// 配合既有 `get snap.bmp`（base64 串口导出）在 Mac 还原查看（M5_PLAN §8 截图法）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "esp_err.h"

// 截取当前活跃 screen → 24bit BMP 写 SD 卡根目录 snap.bmp。
// 必须在 LVGL 任务上下文调用（经 ui_snapshot_request_async 派发）：
// 全屏渲染调用链栈需求大，在 REPL 等小栈任务里直接调会栈溢出。
// SD 未挂载返回 ESP_ERR_NOT_FOUND。
esp_err_t ui_snapshot_to_sdcard(void);

// 异步快照：post 到 UI 消息队列，由 LVGL 任务执行；随后用 ui_snapshot_wait_done 取结果。
esp_err_t ui_snapshot_request_async(void);

// 等待最近一次异步快照完成，返回其执行结果（timeout_ms 内未完成返回 ESP_ERR_TIMEOUT）。
esp_err_t ui_snapshot_wait_done(uint32_t timeout_ms);

// 供 ui_task 消息泵在 LVGL 任务上下文调用（执行快照并给出完成信号）。
void ui_snapshot_execute_from_ui_task(void);

#ifdef __cplusplus
}
#endif
