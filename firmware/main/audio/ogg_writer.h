// Ogg 页封装写入器（RFC 3533）——录音文件落卡的封装层，硬件无关纯逻辑。
//
// 职责：把逻辑包（Ogg packet）按 lacing 规则组装成页，页满自动封页，
// 经回调产出完整页字节；文件 IO 由调用方负责（回调里写卡/写文件）。
// 不碰任何 esp_* 头文件，可在宿主机编译单测。
//
// 页 CRC32 为直连型：多项式 0x04C11DB7，初值 0，无输入/输出反射，异或出 0，
// 覆盖整页（计算时 crc 字段清零）。lacing：段值 255 表示包延续到下一段，
// 包长为 255 的整数倍时以 0 段终结；跨页包的后续页置续传位（0x01）。
#ifndef BUZZ_OGG_WRITER_H
#define BUZZ_OGG_WRITER_H

#include <stddef.h>
#include <stdint.h>

#include "buzz_proto.h" // 错误码约定（BUZZ_OK=0，负值为错）

#ifdef __cplusplus
extern "C" {
#endif

// header_type 标志位（RFC 3533 §1.3.1）
#define BUZZ_OGG_HEADER_CONTINUED 0x01u // 本页首段是上一页未完包的延续
#define BUZZ_OGG_HEADER_BOS 0x02u       // 流首页（beginning of stream）
#define BUZZ_OGG_HEADER_EOS 0x04u       // 流末页（end of stream）

// write_packet 的 flags 入参（仅 BOS/EOS 允许，其余位保留）
#define BUZZ_OGG_FLAG_BOS BUZZ_OGG_HEADER_BOS
#define BUZZ_OGG_FLAG_EOS BUZZ_OGG_HEADER_EOS

// 固定用途参数：16 kHz 单声道 Opus（BUZZLINK 录音格式）
#define BUZZ_OGG_OPUS_HEAD_LEN 19u
#define BUZZ_OGG_OPUS_PRESKIP 312u
#define BUZZ_OGG_OPUS_INPUT_RATE 16000u
#define BUZZ_OGG_OPUS_CHANNELS 1u

// 页组装约束：固定头 27 字节 + 段表最多 255 项；最小页缓冲 = 27 + 255 + 255
#define BUZZ_OGG_PAGE_HEADER_LEN 27u
#define BUZZ_OGG_MAX_SEGMENTS 255u
#define BUZZ_OGG_MAX_SEGMENT_PAYLOAD 255u
#define BUZZ_OGG_PAGE_BUF_MIN (BUZZ_OGG_PAGE_HEADER_LEN + BUZZ_OGG_MAX_SEGMENTS + \
                               BUZZ_OGG_MAX_SEGMENT_PAYLOAD) // 537

// 页产出回调：page 指向 writer 的页缓冲（下一次 write/flush 前有效），ctx 为调用方上下文
typedef void (*buzz_ogg_page_cb_t)(const uint8_t *page, uint32_t page_len, void *ctx);

// Ogg 写入器：缓冲全部由调用方提供（零 malloc）。
// 组装期页缓冲布局为 [27 字节头占位][payload…]，封页时把段表插入中间。
typedef struct {
    uint32_t serial;    // 流序列号（调用方给定，整流一致即可）
    uint64_t granule;   // 最近完成包的 granule（单调校验基准）
    uint32_t page_seq;  // 下一页序号
    uint8_t started;    // 是否已写入首个包（BOS 只允许首包）
    uint8_t finalized;  // EOS 已产出，禁止再写

    uint8_t *page_buf;  // 页组装缓冲（容量 ≥ BUZZ_OGG_PAGE_BUF_MIN）
    uint32_t page_cap;  // 页缓冲容量（决定单页 payload 上限 = cap - 27 - 255）
    uint8_t seg_table[BUZZ_OGG_MAX_SEGMENTS]; // 当前页段表（与 payload 分开存，封页时合并）
    uint8_t num_segments;                     // 当前页已用段数
    uint32_t payload_len;                     // 当前页已装 payload 字节
    uint8_t cur_header;                       // 当前页 header_type（含续传位）
    buzz_ogg_page_cb_t cb;                    // 页产出回调（NULL = 丢弃）
    void *ctx;
} buzz_ogg_writer_t;

// 初始化写入器：serial 由调用方给定；page_buf/page_cap 为页组装缓冲；cb 为页产出回调。
// page_cap 必须 ≥ BUZZ_OGG_PAGE_BUF_MIN，否则返回 BUZZ_ERR_ARG（w 被清零）。
int buzz_ogg_writer_init(buzz_ogg_writer_t *w, uint32_t serial, uint8_t *page_buf,
                         uint32_t page_cap, buzz_ogg_page_cb_t cb, void *ctx);

// 写入一个完整逻辑包。granule = 该包末端的采样计数（单调不减，首包可为 0）。
// flags：BUZZ_OGG_FLAG_BOS 仅允许首包；BUZZ_OGG_FLAG_EOS 标记末包
// （该包所在页封页时置 EOS 位，之后写入器进入 finalized）。
// 包大于当前页剩余空间时自动跨页（后续页置续传位，页 granule 置 -1）。
// 返回 BUZZ_OK；参数非法 / granule 回退 / 已 finalized 返回 BUZZ_ERR_ARG。
int buzz_ogg_write_packet(buzz_ogg_writer_t *w, const uint8_t *packet, uint32_t len,
                          uint64_t granule, uint8_t flags);

// 强制封出当前未满页（页内无未完成包时才有意义；空页不产生输出）。
int buzz_ogg_flush_page(buzz_ogg_writer_t *w);

// 结束流：先封出未满页，再产出一页空 EOS 末页（granule 取末包 granule，单调校验）。
// 若末包已带 BUZZ_OGG_FLAG_EOS 写入，则 EOS 页已产出，本函数为幂等空操作。
// 返回 BUZZ_OK；granule 回退返回 BUZZ_ERR_ARG。
int buzz_ogg_finalize(buzz_ogg_writer_t *w, uint64_t granule);

// 生成 OpusHead 头包内容（19 字节）：version=1, ch=1, preskip=312, rate=16000。
// 返回字节数；缓冲不足返回 BUZZ_ERR_SIZE。
int buzz_ogg_opus_head(uint8_t *out, size_t cap);

// 生成 OpusTags 头包内容：vendor 字符串 + 0 条用户注释。
// vendor 为 NULL 时用默认 "beecord"。返回字节数；缓冲不足返回 BUZZ_ERR_SIZE。
int buzz_ogg_opus_tags(uint8_t *out, size_t cap, const char *vendor);

#ifdef __cplusplus
}
#endif

#endif // BUZZ_OGG_WRITER_H
