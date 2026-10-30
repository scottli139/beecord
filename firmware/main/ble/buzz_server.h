// BuzzLink 设备端协议服务层（M4.2）：字节流喂入 → 重组 → 按 channel/type 分发
// → 业务回调 → 组帧/分片 → 发送回调吐出。
//
// 职责边界（M4_BLE_PLAN §3.1）：
//   - 入向：buzz_server_feed() 喂任意切分的字节流；buzz_parser 完成 magic 同步、
//     帧 CRC 校验、未知 version/type/flags 保留位静默跳过（§10）、坏帧丢弃；
//     buzz_reasm 完成 FRAG 分片重组；本层按 (channel, type) 分发到注册的业务回调。
//   - 出向：回调返回的响应 payload 由本层组帧（session/channel 回显请求帧），
//     超过运行时 max_payload 自动按 §3.3 分片（FRAG 位 + fragIndex/fragCount，
//     每片 data ≤ max_payload-4），逐帧调发送回调。
//   - PING（chan 0x0000）内置处理：回 PONG，payload 原样回显。
//   - 控制通道（chan 0x0001）回调返回 <0 时自动发 ERROR 帧（type 0x7F，
//     JSON {"code":X,"message":"..."}，§9）；文件通道错误由回调自己在
//     响应 payload 里编 status 字节，本层不特殊处理。
//
// 线程模型：单上下文同步处理，不可重入——feed 的调用方、发送回调与业务回调
// 不得重入本层任何 API；§3.4"每 channel 同一时刻最多一个未完成命令"由同步
// 处理天然保证。
// 内存：单实例静态分配（buzz_server_t 由调用方提供存储，零 malloc，内嵌全部
// 缓冲约 5 KB）；DL_CHUNK 等主动推送的 payload 由调用方持有，本层只读。
//
// 本模块硬件无关：不 include 任何 esp_* / NimBLE 头，可宿主单测。
#ifndef BUZZ_SERVER_H
#define BUZZ_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include "buzz_proto.h"
#include "buzz_reasm.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------- 缓冲/容量常量（内嵌于 buzz_server_t，零 malloc） ----------

#define BUZZ_SERVER_REQ_CAP 128u // 入向重组缓冲：请求消息远小于此（DL 请求 ≤33B、JSON 请求 ≤~50B）
#define BUZZ_SERVER_FRAG_SLOTS 16u // 分片位图槽位：同一条消息最多接收的片数
#define BUZZ_SERVER_RESP_CAP 4096u // 出向响应缓冲：§5.2 一页 ≤16 条约 2KB，4KB 留余量

// ---------- §9 错误码（控制通道 ERROR 帧 JSON 的 code 字段） ----------

#define BUZZ_ERR_CODE_BAD_REQUEST 4000 // 帧/参数非法
#define BUZZ_ERR_CODE_FORBIDDEN 4003   // 禁止操作
#define BUZZ_ERR_CODE_NOT_FOUND 4004   // 不存在
#define BUZZ_ERR_CODE_BUSY 4009        // 忙（如下载进行中又收 DL_BEGIN）
#define BUZZ_ERR_CODE_TOO_LARGE 4130   // 消息超上限
#define BUZZ_ERR_CODE_STORAGE 4500     // 存储错误
#define BUZZ_ERR_CODE_INTERNAL 4999    // 内部错误

// 本层自有错误码（其余复用 buzz_proto.h 的 BUZZ_OK/BUZZ_ERR_NULL/BUZZ_ERR_ARG/BUZZ_ERR_SIZE）
#define BUZZ_SERVER_ERR_SEND (-10) // 发送回调报告失败（流控背压/断连等，善后归调用方）

// ---------- 回调 ----------

// 出向发送：一帧一次同步调用（已按 max_payload 分片）。
// frame 视图仅在回调体内有效，回调须立即拷贝或发送；返回 0 成功，<0 失败。
typedef int (*buzz_send_fn_t)(void *ctx, const uint8_t *frame, size_t len);

// 业务命令回调（控制/文件通道共用签名；req 为重组后的完整请求帧视图）：
//   ctx       注册时给出的上下文
//   req       请求帧：type/session/channel 为帧头字段，payload/length 为完整消息；
//             视图仅在回调体内有效，禁止留存指针
//   resp      响应写入缓冲（容量 resp_cap）；走 ERROR 帧路径时内容被本层覆盖，无需理会
//   resp_cap  响应缓冲容量（= BUZZ_SERVER_RESP_CAP）
//   err_code  仅返回 <0 时读取：§9 错误码；未填（0）按 BUZZ_ERR_CODE_INTERNAL 处理
//   err_msg   仅返回 <0 时读取：JSON 安全常量字符串（无引号/反斜杠/控制字符）；
//             为空按 "internal"
// 返回 ≥0：响应字节数（必须 ≤ resp_cap），本层按请求 type 映射响应 type
//         （INFO_GET→INFO、LIST_REQ→LIST_RESP、FILE_INFO_REQ→FILE_INFO_RESP、
//         FILE_DELETE_REQ→FILE_DELETE_RESP、DL_BEGIN_REQ→DL_BEGIN_RESP、
//         DL_END_REQ/DL_ABORT→DL_END_RESP），回显 session/channel 后发送；
// 返回 <0：本层自动发 ERROR 帧（type 0x7F、控制通道、session 回显）。
typedef int (*buzz_cmd_handler_t)(void *ctx, const buzz_frame_t *req, uint8_t *resp,
                                  size_t resp_cap, int *err_code, const char **err_msg);

// 回调注册表：init 时一次性注入（结构体拷贝）；任一项可为 NULL。
typedef struct {
    buzz_send_fn_t send; // 出向发送
    void *send_ctx;
    buzz_cmd_handler_t control; // JSON 控制通道（INFO_GET/LIST_REQ/FILE_INFO_REQ/FILE_DELETE_REQ）
    void *control_ctx;
    buzz_cmd_handler_t file; // 二进制文件通道（DL_BEGIN_REQ/DL_END_REQ/DL_ABORT）
    void *file_ctx;
} buzz_server_io_t;

// ---------- 实例（调用方提供存储；内嵌全部缓冲） ----------

typedef struct {
    buzz_parser_t parser; // 字节流 → 帧（CRC 校验/容错在解析器内兜底）
    buzz_reasm_t reasm;   // 帧 → 完整消息（FRAG 分片重组）
    uint8_t req_buf[BUZZ_SERVER_REQ_CAP];             // reasm 重组缓冲
    uint8_t frag_bitmap[BUZZ_SERVER_FRAG_SLOTS / 8u]; // reasm 片到位位图
    uint8_t resp[BUZZ_SERVER_RESP_CAP]; // 业务响应 / ERROR JSON 缓冲
    uint8_t tx[BUZZ_FRAME_HEADER_LEN + BUZZ_FRAME_MAX_PAYLOAD + BUZZ_FRAME_CRC_LEN]; // 组帧暂存
    uint16_t max_payload;  // 运行时单帧 payload 上限（§3.3；set_max_payload 调整）
    buzz_server_io_t io;   // 回调注册表（init 拷贝）
} buzz_server_t;

// 初始化实例：绑定缓冲与回调表（结构体拷贝）、复位全部状态。
// 单实例设计：同一时刻只允许一个活跃实例（缓冲内嵌，语义上即"设备"本身）。
// s/io 为 NULL 直接返回（io 任一项可为 NULL：未注册控制/文件回调时对应命令回
// ERROR 帧 4999；send 为 NULL 时所有出向发送静默失败）。
void buzz_server_init(buzz_server_t *s, const buzz_server_io_t *io);

// 喂入任意切分/合并的入向字节流（Cmd Write 特征原始数据）。
// 同步完成解析/重组/分发/组帧/发送回调；返回 BUZZ_OK，
// s/data 非法返回 BUZZ_ERR_NULL。重组违规（分片不一致、超缓冲）静默丢弃
// 该消息并保持流可用（§10 精神：不报错、不断连）。
int buzz_server_feed(buzz_server_t *s, const uint8_t *data, size_t len);

// 主动组帧发送（设备推送：INFO 用 session 0，DL_CHUNK 沿用 DL_BEGIN 的 session）。
// 超 max_payload 自动按 §3.3 分片；逐帧经发送回调。
// DL_CHUNK 等天然小块消息调用方应保证不触发分片（§3.3）。
// 返回 BUZZ_OK；参数非法返回 BUZZ_ERR_NULL/BUZZ_ERR_ARG，超协议上限
// （64 KB / 分片数溢出 u16）返回 BUZZ_ERR_SIZE，发送回调失败返回
// BUZZ_SERVER_ERR_SEND（分片中途失败即中止）。
int buzz_server_send(buzz_server_t *s, uint8_t type, uint8_t session, uint16_t channel,
                     const uint8_t *payload, size_t len);

// 设置运行时单帧 payload 上限（M4.3 按连接实际 att_mtu-15 设置；§3.3）。
// 合法区间 [BUZZ_FRAG_HEADER_LEN+1, BUZZ_FRAME_MAX_PAYLOAD]（下限保证分片每片
// 至少 1 字节数据）；越界返回 BUZZ_ERR_ARG。默认 BUZZ_FRAME_MAX_PAYLOAD（232）。
int buzz_server_set_max_payload(buzz_server_t *s, uint16_t n);

// ---------- 极简 JSON 助手（D2：入站请求解析与 M4.3 命令实现复用） ----------

// 仅支持扁平对象的 string/number 值提取：容忍空白与字段顺序，未知字段
// （含 array/object 值）正确跳过；只匹配第 1 层键，嵌套对象内键不查找；
// 键重复时以先出现者为准。JSON 生成用 snprintf，本层不提供生成器。
#define BUZZ_JSON_ERR_NOT_FOUND (-1) // 键不存在
#define BUZZ_JSON_ERR_MALFORMED (-2) // JSON 结构非法或值类型与请求不符

// 提取整数（strtol，十进制；值后必须是合法分隔符，拒绝 3.14/12x 等畸形）。
// 返回 BUZZ_OK / BUZZ_ERR_NULL / BUZZ_JSON_ERR_NOT_FOUND /
// BUZZ_JSON_ERR_MALFORMED。
int buzz_json_get_int(const char *json, const char *key, long *out);

// M5.4（v0.2 TIME_SET）：int64 解析（epochMs 超 32 位 long 范围）。
// 返回码同 buzz_json_get_int。
int buzz_json_get_i64(const char *json, const char *key, int64_t *out);

// 提取字符串（容忍 \\ 与 \" 转义；cap 含 NUL）。buf 在成功时必定 NUL 结尾。
// 返回 BUZZ_OK / BUZZ_ERR_NULL / BUZZ_ERR_SIZE（cap 不足）/
// BUZZ_JSON_ERR_NOT_FOUND / BUZZ_JSON_ERR_MALFORMED。
int buzz_json_get_str(const char *json, const char *key, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif // BUZZ_SERVER_H
