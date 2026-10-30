// Beecord 设置框架 ESP NVS 适配（settings_esp.h/.c，M5.4）。
// 把 settings 的存储回调接到 nvs_flash（命名空间 "bee_cfg"）。
// 键：settings_id_t 数值；u8 项用 nvs_set_u8，float 项用 nvs_set_float。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

#include "settings.h"

// 初始化：打开 NVS 命名空间并向 settings_init 注册适配器（幂等）。
// 须在 nvs_flash_init 之后调用。
esp_err_t settings_esp_init(void);

// 供调试：把当前全部设置项打日志
void settings_esp_dump(void);

#ifdef __cplusplus
}
#endif
