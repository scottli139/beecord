// Beecord TF 卡存储（M2 起完善：录音文件写入、索引管理）。
// 当前为最小探测实现：挂载卡、打印卡信息与根目录列表——用于 M1 到货自检。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 探测 TF 卡：挂载（BSP SDMMC 1-bit）、打印卡信息、列根目录。
// 成功返回 ESP_OK 且保持挂载（后续里程碑直接复用）；失败返回 BSP/FATFS 错误码。
esp_err_t buzz_sdcard_probe(void);

// M5.3：挂载状态查询（UI 无卡红条/禁录音用）。probe 成功前均为 false。
bool buzz_sdcard_mounted(void);

// 最近一次挂载/格式化结果，供 UI 区分无卡、存储错误和格式化候选。
esp_err_t buzz_sdcard_last_error(void);

// 仅清理符合录音命名规则的 0 字节普通文件；其他文件与目录不动。
uint32_t buzz_sdcard_cleanup_orphans(void);

// 仅供显式确认后的服务任务调用；绝不自动格式化。
esp_err_t buzz_sdcard_format(void);

#ifdef __cplusplus
}
#endif
