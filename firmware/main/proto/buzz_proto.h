// BuzzLink 协议层 —— 帧格式常量、编码与字节流解析。
//
// 帧布局（多字节小端，头 10 字节 + payload + CRC16 2 字节）：
//   magic(2)=BE EC | version(1) | type(1) | session(1) | flags(1) | channel(2)
//   | length(2)=payload 字节数 | payload | crc16(2)
// CRC 覆盖 version..payload（偏移 2 至 10+length）。协议细节见 docs/BUZZLINK.md。
//
// 本模块仅做帧级编解码（D1）：JSON 消息层不在此处理。硬件无关，可宿主单测。
#ifndef BUZZ_PROTO_H
#define BUZZ_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------- 协议常量（单点定义，BUZZLINK.md §3/§5.1） ----------

#define BUZZ_MAGIC0 0xBEu // 帧魔数第 1 字节
#define BUZZ_MAGIC1 0xECu // 帧魔数第 2 字节

#define BUZZ_VERSION_1 0x01u // 当前协议版本（§10：单调递增，未知版本忽略该帧）

#define BUZZ_FLAG_FRAG 0x01u // flags bit0 = 分片帧；其余位保留，置位即忽略该帧

// 帧类型（§5.1）
#define BUZZ_TYPE_PING 0x01u          // C→D 保活，channel 0x0000，无 payload
#define BUZZ_TYPE_PONG 0x02u          // D→C 回显 PING payload
#define BUZZ_TYPE_INFO 0x03u          // D→C 设备信息 JSON（可主动推，session=0）
#define BUZZ_TYPE_INFO_GET 0x04u      // C→D 请求设备信息
#define BUZZ_TYPE_LIST_REQ 0x10u      // C→D 文件列表请求 JSON
#define BUZZ_TYPE_LIST_RESP 0x11u     // D→C 文件列表页 JSON
#define BUZZ_TYPE_FILE_INFO_REQ 0x20u // C→D 文件元数据请求 JSON
#define BUZZ_TYPE_FILE_INFO_RESP 0x21u
#define BUZZ_TYPE_DL_BEGIN_REQ 0x30u  // C→D 下载开始 [idLen u8][id…]
#define BUZZ_TYPE_DL_BEGIN_RESP 0x31u // D→C [status u8][chunkDataLen u16][totalChunks u32][size u32][fileCrc16 u16]
#define BUZZ_TYPE_DL_CHUNK 0x32u      // D→C [seq u16][data…]，不分片
#define BUZZ_TYPE_DL_END_REQ 0x33u    // C→D 结束校验 [idLen u8][id…]
#define BUZZ_TYPE_DL_END_RESP 0x34u   // D→C [status u8][fileCrc16 u16]
#define BUZZ_TYPE_DL_ABORT 0x35u      // C→D 中止 [idLen u8][id…]
#define BUZZ_TYPE_FILE_DELETE_REQ 0x40u  // P1 可选
#define BUZZ_TYPE_FILE_DELETE_RESP 0x41u // P1 可选
#define BUZZ_TYPE_ERROR 0x7Fu         // D→C 异步错误 JSON

// 通道（§4）
#define BUZZ_CHANNEL_LINK 0x0000u    // 链路：空载荷（PING/PONG）
#define BUZZ_CHANNEL_CONTROL 0x0001u // 控制：JSON
#define BUZZ_CHANNEL_FILE 0x0002u    // 文件：二进制下载流水

// status 字节（§5.1 DL_BEGIN_RESP / DL_END_RESP）
#define BUZZ_STATUS_OK 0u
#define BUZZ_STATUS_NOT_FOUND 1u
#define BUZZ_STATUS_ABORTED 2u
#define BUZZ_STATUS_BUSY 3u
#define BUZZ_STATUS_BAD_REQUEST 4u
#define BUZZ_STATUS_STORAGE_ERROR 5u
#define BUZZ_STATUS_INTERNAL 6u

// 上限（§3.3/§5.2/§6.2）
#define BUZZ_FRAME_HEADER_LEN 10u                  // 帧头固定字节数
#define BUZZ_FRAME_CRC_LEN 2u                      // 帧尾 CRC 字节数
#define BUZZ_FRAME_MAX_PAYLOAD 232u                // 单帧 payload 上限（MTU247-3-12）
#define BUZZ_FRAG_HEADER_LEN 4u                    // 分片头 fragIndex(2)+fragCount(2)
#define BUZZ_FRAG_MAX_DATA (BUZZ_FRAME_MAX_PAYLOAD - BUZZ_FRAG_HEADER_LEN) // 228
#define BUZZ_MSG_MAX_LEN (64u * 1024u)             // 消息（含分片重组）总上限
#define BUZZ_CHUNK_DATA_LEN_DEFAULT 224u           // DL_BEGIN_RESP 默认块长（§6.2）
#define BUZZ_FILE_ID_MAX_LEN 32u                   // 文件 ID 上限（§5.2）

// ---------- 错误码（BUZZ_OK=0，负值为错） ----------

#define BUZZ_OK 0
#define BUZZ_ERR_NULL (-1)  // 必传指针为空
#define BUZZ_ERR_ARG (-2)   // 参数非法（未知 type/flags 保留位置位/分片参数越界等）
#define BUZZ_ERR_SIZE (-3)  // 输出缓冲不足 / 超协议上限
#define BUZZ_ERR_PROTO (-4) // 协议违规（分片不一致等，调用方应放弃当前消息）

// ---------- 帧视图 ----------

// 帧/消息的解码视图：payload 指向提供方内部缓冲，仅在交付回调/返回后短期内有效。
// length 用 32 位：单帧 ≤232，但分片重组消息可达 64 KB，重组结果复用本视图交付。
typedef struct {
    uint8_t version;
    uint8_t type;
    uint8_t session;
    uint8_t flags;
    uint16_t channel;
    uint32_t length; // payload 字节数
    const uint8_t *payload;
} buzz_frame_t;

// ---------- 编码 ----------

// 编码一帧到调用方缓冲（append 式，从 out[0] 起写）。
// 校验：type 已知、flags 无保留位、payload_len ≤ BUZZ_FRAME_MAX_PAYLOAD。
// 返回整帧字节数（>0）；参数非法返回 BUZZ_ERR_ARG，缓冲不足返回 BUZZ_ERR_SIZE。
int buzz_frame_encode(uint8_t *out, size_t out_cap, uint8_t type, uint8_t session,
                      uint8_t flags, uint16_t channel, const uint8_t *payload,
                      uint16_t payload_len);

// 编码分片帧：payload = fragIndex(2) fragCount(2) data…（§3.3）。
// 校验：frag_count ≥ 1、frag_index < frag_count、data_len ≤ BUZZ_FRAG_MAX_DATA。
int buzz_frag_encode(uint8_t *out, size_t out_cap, uint8_t type, uint8_t session,
                     uint16_t channel, uint16_t frag_index, uint16_t frag_count,
                     const uint8_t *data, uint16_t data_len);

// 从 FRAG 帧解析分片头（配合 buzz_reasm 使用）。
// 返回 BUZZ_OK 并给出 data/data_len 视图（指向 frame->payload 内部，随 frame 失效）；
// 非 FRAG 帧或 payload 短于分片头返回 BUZZ_ERR_ARG。
int buzz_frag_parse(const buzz_frame_t *frame, uint16_t *frag_index, uint16_t *frag_count,
                    const uint8_t **data, uint16_t *data_len);

// type 是否协议已知（§10：未知 type 的帧接收方忽略）。
bool buzz_type_is_known(uint8_t type);

// ---------- 字节流解析 ----------

// 解析完成回调：frame 视图仅在回调体内有效；回调内禁止重入 buzz_parser_feed。
typedef void (*buzz_frame_cb_t)(const buzz_frame_t *frame, void *ctx);

typedef enum {
    BUZZ_PARSE_SYNC = 0, // 寻找魔数
    BUZZ_PARSE_HEAD,     // 收帧头剩余 8 字节（version..length）
    BUZZ_PARSE_DISCARD,  // 按 length 丢弃非法帧的 payload+crc，保持流对齐
    BUZZ_PARSE_PAYLOAD,  // 收 payload
    BUZZ_PARSE_CRC       // 收帧尾 CRC
} buzz_parse_state_t;

// 解析器：缓冲按协议单帧最大值内嵌，结构体由调用方提供（零 malloc）。
// 任意时刻内部缓冲仅存当前帧，回调返回后即失效。
typedef struct {
    buzz_parse_state_t state;
    uint8_t header[BUZZ_FRAME_HEADER_LEN]; // [0..1]=magic，[2..9]=version..length
    uint8_t header_len;                    // 已收帧头字节数
    uint8_t payload[BUZZ_FRAME_MAX_PAYLOAD];
    uint16_t payload_len;  // 已收 payload 字节数
    uint16_t expect_len;   // 帧头声明的 payload 长度
    uint32_t discard_left; // DISCARD 状态剩余字节数
    uint8_t crc[BUZZ_FRAME_CRC_LEN];
    uint8_t crc_len;
    buzz_frame_cb_t cb; // 可为 NULL：合法帧直接丢弃（仅做流校验）
    void *ctx;
} buzz_parser_t;

// 初始化解析器；cb 为合法帧回调（NULL 表示丢弃）。
void buzz_parser_init(buzz_parser_t *parser, buzz_frame_cb_t cb, void *ctx);

// 喂入任意切分/合并的字节流；合法帧通过回调同步交付。
// 容忍：垃圾前缀（magic 重同步）、CRC 错帧（弃帧重同步）、
// 未知 version/type/flags 保留位（忽略该帧，不报错，§10）。
// 返回 BUZZ_OK；parser/data 为空返回 BUZZ_ERR_NULL。
int buzz_parser_feed(buzz_parser_t *parser, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // BUZZ_PROTO_H
