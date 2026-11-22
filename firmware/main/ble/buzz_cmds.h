// BuzzLink BLE 命令服务（M4.3）：把 buzz_server 协议服务层接到设备侧——
// GATT 写 → FreeRTOS 队列 → buzz 服务任务 → server 分发（控制通道 INFO_GET /
// LIST_REQ / FILE_INFO_REQ / TIME_SET_REQ；FILE_DELETE_REQ 为 P1 占位）→ 响应经
// buzz_gatt_notify_send 回推。含订阅主动 INFO（§6.1）、MTU 自适应分片上限
// （D3：att_mtu-15）、空闲 60s 主动断开（D10）。
//
// 线程模型（M4_BLE_PLAN §3.2）：
//   - GATT 写回调 / BLE 事件 = NimBLE host 任务上下文：只拷贝推进队列，绝不做
//     文件 I/O、绝不直接调 buzz_server；
//   - buzz 服务任务（本模块，优先级 4：低于音频采集 5、高于 REPL）：唯一调用
//     buzz_server_feed / buzz_server_send 的上下文，文件 I/O 在此发生；
//   - 单连接（D9）：全部状态静态持有，零运行期 malloc（队列/任务启动期创建）。
//
// 文件存储经 file_index（读取回调注入，本模块提供 FATFS/VFS 五件套实现）。
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动本地命令服务：初始化 file_index + 设备模型 + buzz_server、创建输入队列与
// 服务任务。可在 buzz_ble_init() 失败后调用；此时本地列表/设置/格式化仍可用。
// 失败返回非 ESP_OK（已记日志）。线程安全：仅 app_main 上下文调用。
esp_err_t buzz_cmds_init(void);

typedef enum {
    BUZZ_LOCAL_SETTING_SCREEN_OFF = 0,
    BUZZ_LOCAL_SETTING_QUALITY,
    BUZZ_LOCAL_SETTING_BEEP,
} buzz_local_setting_t;

typedef enum {
    BUZZ_FORMAT_REQUEST = 0,
    BUZZ_FORMAT_CANCEL,
    BUZZ_FORMAT_CONFIRM,
} buzz_format_action_t;

esp_err_t buzz_cmds_request_refresh(void);
esp_err_t buzz_cmds_cycle_setting(buzz_local_setting_t setting);
esp_err_t buzz_cmds_format_action(buzz_format_action_t action);

#ifdef __cplusplus
}
#endif
