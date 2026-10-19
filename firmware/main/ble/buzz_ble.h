// Beecord BLE 基础设施（M4.1）：NimBLE 初始化、BCD-XXXX 广播、
// 连接/断开/MTU/订阅事件处理与状态查询接口。
//
// 线程模型：buzz_ble_init 在 app_main 上下文调用一次；此后全部回调与事件
// 在 NimBLE host 任务上下文执行（M4.2 起写路径只推队列、不做文件 I/O）。
// 状态查询接口供 REPL 任务只读，无锁（单连接 + 事件内更新）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 未连接时 buzz_ble_conn_handle() 的返回值。
#define BUZZ_BLE_CONN_HANDLE_NONE 0xffffu

// 初始化 NimBLE 协议栈、注册 BuzzLink GATT 服务并启动广播。
// 设备名在内部按 BT MAC 派生（BCD-XXXX，BUZZLINK.md §2.2）。
// 失败返回非 ESP_OK（已记日志）；调用方应降级继续（REPL 照常可用）。
// 仅在 app_main 上下文调用一次。
esp_err_t buzz_ble_init(void);

// 设备名字符串（如 "BCD-A12B"），指向内部静态缓冲，永不为 NULL。
const char *buzz_ble_device_name(void);

// 当前是否有中心设备连接。
bool buzz_ble_is_connected(void);

// 当前连接句柄；未连接返回 BUZZ_BLE_CONN_HANDLE_NONE。
uint16_t buzz_ble_conn_handle(void);

// 当前协商后 ATT MTU；未连接或未协商返回 BLE 默认值 23。
uint16_t buzz_ble_mtu(void);

// Event Notify 特征是否被中心设备订阅。
bool buzz_ble_notify_subscribed(void);

// ---------- 事件订阅（M4.3：buzz_cmds 订阅连接生命周期事件） ----------

typedef enum {
    BUZZ_BLE_EVT_CONNECT = 0, // 已连接（状态变量已更新，可安全查询句柄/MTU 初值）
    BUZZ_BLE_EVT_DISCONNECT,  // 已断开（查询接口已回落默认值）
    BUZZ_BLE_EVT_MTU,         // ATT MTU 协商完成（新值经 buzz_ble_mtu() 查询）
    BUZZ_BLE_EVT_SUBSCRIBE,   // Notify 订阅态变化（新态经 buzz_ble_notify_subscribed() 查询）
} buzz_ble_evt_t;

// 事件回调：在 NimBLE host 任务上下文同步执行（GAP 事件内）。
// 回调内禁止做文件 I/O、禁止调用 buzz_server 任何 API——只许快速置标志/
// 推队列；状态值用上方查询接口读取。仅支持单一订阅方（buzz_cmds）。
typedef void (*buzz_ble_evt_cb_t)(buzz_ble_evt_t evt, void *ctx);

// 注册事件回调（NULL 清除）；重复调用覆盖。可在 host 任务运行前/后调用。
void buzz_ble_set_evt_cb(buzz_ble_evt_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
