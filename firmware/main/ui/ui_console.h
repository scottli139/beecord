// UI 相关 REPL 命令注册（ui_console.h）：`screen off|on`、`ui snap`。
// 由 console_cmd_start_repl 在 REPL 启动时调用一次；UI 未初始化时命令报错。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

esp_err_t ui_console_register(void);

#ifdef __cplusplus
}
#endif
