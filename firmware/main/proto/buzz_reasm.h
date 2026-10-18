// BuzzLink 协议层 —— FRAG 分片重组（§3.3）。
//
// 喂入解析器交付的帧：FRAG 帧收集拼齐后交付完整消息；非 FRAG 帧原样透传
// （如分片响应期间到达链路通道的 PING）。同一时刻只处理一条分片消息，
// 与协议"每通道一个未完成命令"的并发模型一致（§3.4）。
//
// 缓冲全部由调用方提供（零 malloc）：消息缓冲 + 片到位位图 + 结构体。
// 硬件无关，可宿主单测。
#ifndef BUZZ_REASM_H
#define BUZZ_REASM_H

#include <stdint.h>

#include "buzz_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;         // 消息重组缓冲（调用方提供）
    uint32_t buf_cap;     // 缓冲容量（字节）
    uint8_t *bitmap;      // 片到位位图（调用方提供，≥ (bitmap_bits+7)/8 字节）
    uint32_t bitmap_bits; // 位图容量（片数）

    uint8_t active;    // 是否有进行中的消息
    uint8_t type;      // 进行中的消息：type
    uint8_t session;   // session
    uint16_t channel;  // channel
    uint16_t stride;   // 非末片数据长（首片非末片到达时确定；0 = 未确定）
    uint16_t frag_count;
    uint16_t last_len; // 末片数据长（末片到达时确定）
    uint32_t received; // 已收片数

    // 末片先于任何非末片到达时的暂存（此时 stride 未定、无法计算落位偏移）
    uint8_t stashed;
    uint16_t stash_len;
    uint8_t stash[BUZZ_FRAG_MAX_DATA];

    buzz_frame_t msg; // 拼齐后填写的消息视图（payload 指向 buf）
} buzz_reasm_t;

// 绑定调用方缓冲。位图容量决定可同时接收的最大片数（frag_count 超位图即拒绝）。
void buzz_reasm_init(buzz_reasm_t *reasm, uint8_t *buf, uint32_t buf_cap,
                     uint8_t *bitmap, uint32_t bitmap_bits);

// 放弃进行中的消息（收到 DL_ABORT、协议违规或上层超时时调用）。
void buzz_reasm_reset(buzz_reasm_t *reasm);

// 喂入一帧：
// - 非 FRAG 帧：*out = 该帧本身，立即返回 BUZZ_OK（透传，不影响重组状态）；
// - FRAG 帧：收集，未拼齐时 *out = NULL 返回 BUZZ_OK；
//   拼齐时 *out 指向内部消息视图（flags=0，payload=buf，length=总字节数）。
// 违规（分片 type/session/channel 不一致、片长与 stride 矛盾、消息超 64 KB）
// 返回 BUZZ_ERR_PROTO 并复位状态；消息超调用方缓冲返回 BUZZ_ERR_SIZE 并复位。
// 重复片幂等：已收到的片再次到达返回 BUZZ_OK、*out = NULL。
// 返回的视图在下一次 buzz_reasm_feed 前有效。
int buzz_reasm_feed(buzz_reasm_t *reasm, const buzz_frame_t *frame, const buzz_frame_t **out);

#ifdef __cplusplus
}
#endif

#endif // BUZZ_REASM_H
