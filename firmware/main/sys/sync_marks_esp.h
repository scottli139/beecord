// Beecord 已同步标记 ESP NVS 适配（sync_marks_esp.h/.c，M5.4）。
// blob 存 nvs_flash 命名空间 "bee_sync" 键 "marks"；加载校验失败按空集处理。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

#include "sync_marks.h"

// 初始化并向 sync_marks_init 注册适配器（幂等）。须在 nvs_flash_init 之后调用。
esp_err_t sync_marks_esp_init(void);

#ifdef __cplusplus
}
#endif
