// Beecord GATT 服务表（M4.1）：BuzzLink 三 UUID 服务（BUZZLINK.md §2.1）与
// Cmd Write 特征写回调（M4.1 仅日志，M4.2 起喂 buzz_server 输入队列）。
//
// UUID 常量按 NimBLE 小端字节序倒序填写（BLE_UUID128_INIT 宏约定），
// 与规范文本的对应关系见 buzz_gatt.c 注释。
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "host/ble_uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

// BuzzLink Service UUID（小端字节序存储；规范文本等值于
// 0E242009-7EF4-4AE0-B3B0-E3A59E58D47C）。广播 AD 的 complete UUID128 list
// 与本服务表共用此常量，保证全固件唯一源。
extern const ble_uuid128_t buzz_gatt_svc_uuid;

// 注册 BuzzLink GATT 服务表（ble_gatts_count_cfg + ble_gatts_add_svcs）。
// 须在 nimble host 启动前调用一次（buzz_ble_init 内部调用）。
// 失败返回 ESP_FAIL（NimBLE 返回码已记日志）。
esp_err_t buzz_gatt_register(void);

// Event Notify 特征的 value handle（notify 发送用，M4.4 起）。
// 完成首次 BLE 同步（host 重置并注册服务）前为 0。
uint16_t buzz_gatt_event_notify_handle(void);

#ifdef __cplusplus
}
#endif
