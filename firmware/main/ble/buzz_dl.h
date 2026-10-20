// BuzzLink 文件下载推流（M4.4）：文件通道（0x0002）状态机 + DL_CHUNK 推流环。
//
// 状态机（单连接单实例，全部静态，零运行期 malloc）：
//
//   IDLE ──DL_BEGIN(ok)──▶ STREAMING ──末块入队──▶ WAIT_END ──DL_END──▶ IDLE
//     ▲                        │  ▲                      │
//     │                        │  └──── NOTIFY_TX 事件泵醒 │ DL_ABORT
//     └──── DL_ABORT / 断连 / 发送失败 ◀──────────────────┘
//
//   - DL_BEGIN 时已有活动传输（STREAMING/WAIT_END）→ DL_BEGIN_RESP status=3 busy
//   - DL_END 无活动传输或状态/ID 不符 → DL_END_RESP status=4 bad_request
//   - DL_ABORT 匹配当前传输 → DL_END_RESP status=2，传输作废
//   - 断连（buzz_cmds 收到 DISCONNECT 调 buzz_dl_cancel）立即作废，不发送任何帧
//   - 录音中（D8）DL_BEGIN → status=3 busy
//
// 推流与流控（D4）：buzz_dl_pump() 只在 buzz 服务任务上下文调用——逐块
// read_at → 组 DL_CHUNK 帧（buzz_server_send 同步编码+发送，mbuf 已拷贝，
// 发送缓冲即可复用）→ notify 入队；TX 池耗尽（ENOMEM，mbuf 被未完成的
// 发送占用）时 pump 返回挂起，由服务任务短滴答轮询续发（本 NimBLE 移植的
// NOTIFY_TX 事件在"入队尝试"时即触发而非发送完成时，不能作完成信用，
// 故采用轮询；详见 M4.4 报告）。断连由 pump 内连接态检查识别并作废传输。
//
// 线程模型：BEGIN/END/ABORT 处理与 pump 均在服务任务上下文（server 分发
// 同步调用 + 任务主动泵）；buzz_ble 回调上下文只许调 buzz_dl_cancel。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buzz_server.h" // buzz_frame_t / buzz_server_t

#ifdef __cplusplus
extern "C" {
#endif

// 绑定协议服务实例（DL_CHUNK 主动推送经 buzz_server_send）并复位状态机。
// 由 buzz_cmds_init 在 buzz_server_init 之后调用一次；server 存活期须不短于本模块。
// 参数非法返回 ESP_ERR_INVALID_ARG。
int buzz_dl_init(buzz_server_t *server);

// 文件通道请求分发（注册为 buzz_server 的 file 回调）：处理 DL_BEGIN_REQ /
// DL_END_REQ / DL_ABORT，响应为 13B/3B 定长二进制（status 语义见 buzz_proto.h）。
// 文件通道错误一律在 payload 内编 status 字节（本层不返回 <0，server 不会
// 发 JSON ERROR 帧到控制通道）。返回响应字节数。
int buzz_dl_handle_request(void *ctx, const buzz_frame_t *req, uint8_t *resp,
                           size_t resp_cap, int *err_code, const char **err_msg);

// 推流泵（仅服务任务上下文）：STREAMING 中逐块发送直到发完（→ WAIT_END）、
// notify 入队被拒（TX 池满，背压挂起等 NOTIFY_TX）或出错（传输作废）。
// 返回本次成功入队的块数（0 = 无进展：非 STREAMING 或背压挂起）；<0 = 传输已作废。
// DL_BEGIN 被服务任务处理完后会立即泵一次，尽快出首块。
int buzz_dl_pump(void);

// 作废当前传输并释放文件句柄（不发送任何帧）：断连时由 buzz_cmds 调用。
// 幂等；任意上下文可调用（仅触碰本模块静态状态，无 IO 发送）。
void buzz_dl_cancel(void);

// 是否有活动传输（STREAMING / WAIT_END）。
bool buzz_dl_busy(void);

#ifdef __cplusplus
}
#endif
