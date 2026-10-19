// Beecord GATT 服务表（M4.1）：BuzzLink 三 UUID 服务（BUZZLINK.md §2.1）、
// Cmd Write 特征写回调（M4.3 起经注册回调转交 buzz 服务任务）与
// Event Notify 发送 API（buzz_server 出向）。
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

// ---------- Cmd Write 写路径回调（M4.3：GATT 写 → buzz 服务任务输入队列） ----------

// 写数据回调：NimBLE host 任务上下文同步触发（每写一次回调一次，data 为完整
// 写载荷视图、仅在回调体内有效）。回调内禁止做文件 I/O / 调用 buzz_server
// API——只许拷贝推进队列；超长写（> cap 约定 256B）直接丢弃并记日志。
typedef void (*buzz_gatt_write_cb_t)(const uint8_t *data, uint16_t len, void *ctx);

// 注册写回调（NULL 恢复 M4.1 仅日志行为）；重复调用覆盖。单一订阅方。
void buzz_gatt_set_write_cb(buzz_gatt_write_cb_t cb, void *ctx);

// ---------- Event Notify 发送（M4.3） ----------

// 经 Event Notify 特征发送一帧（notify，无响应）。
// 发送前检查：服务已注册（val handle 有效）→ 已连接 → 对端已订阅；
// 未满足返回相应错误码（BLE_HS_ENOTCONN / BLE_HS_EINVAL，后者亦用于
// "未订阅"——本 NimBLE 版本无专用错误码），调用方据此停止推流。mbuf 分配失败返回 BLE_HS_ENOMEM（NimBLE 流控背压，
// 由 NOTIFY_TX 事件释放后重试，D4）。返回值为 NimBLE 返回码（0 = 成功）。
// 可在任意任务上下文调用（内部持 host 锁）。
int buzz_gatt_notify_send(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif
