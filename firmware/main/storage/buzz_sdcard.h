// Beecord TF 卡存储（M2 起完善：录音文件写入、索引管理）。
// 当前为最小探测实现：挂载卡、打印卡信息与根目录列表——用于 M1 到货自检。
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 探测 TF 卡：挂载（BSP SDMMC 1-bit）、打印卡信息、列根目录。
// 成功返回 ESP_OK 且保持挂载（后续里程碑直接复用）；失败返回 BSP/FATFS 错误码。
esp_err_t buzz_sdcard_probe(void);

#ifdef __cplusplus
}
#endif
