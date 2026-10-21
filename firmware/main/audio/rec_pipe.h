// 硬件无关录音管线（M3）：帧切片（跨读块攒 320 采样）→ Opus 编码 → Ogg 封装 →
// 页回调输出字节。会话模型 init（写 OpusHead/OpusTags 两页）→ feed（PCM 块）→
// finish（丢尾帧 + EOS 封页）。
//
// 零 malloc（缓冲全部由调用方提供或内嵌在结构体）、零 esp_* 依赖，
// 目标侧页回调里 fwrite 写 TF 卡，宿主侧 fwrite 写测试文件。
#pragma once

#include <stdint.h>

#include "buzz_proto.h" // 错误码约定（BUZZ_OK=0，负值为错）
#include "ogg_writer.h"
#include "opus_enc.h"

#ifdef __cplusplus
extern "C" {
#endif

// 编码参数（M3 拍板：44kbps VBR / complexity 5 / AUDIO 模式）
#define BUZZ_REC_PIPE_BITRATE 44000
#define BUZZ_REC_PIPE_COMPLEXITY 5

// 页缓冲建议容量（≥ BUZZ_OGG_PAGE_BUF_MIN；4KB ≈ 28 包/页 ≈ 0.56s 音频）
#define BUZZ_REC_PIPE_PAGE_BUF_SIZE 4096u

// 会话统计（finish 后定稿）
typedef struct {
    uint32_t frames;      // 已封入 Ogg 的 Opus 帧数（不含被丢弃的尾帧）
    uint64_t samples_in;  // 累计输入采样（含尾帧丢弃部分）
    uint64_t bytes_out;   // 页回调累计输出字节
    uint32_t encode_errs; // 编码失败累计（feed 遇错即返回，调用方决定是否中止）
} buzz_rec_pipe_stats_t;

// 管线对象：除页缓冲外全部内嵌；调用方静态/栈分配，init 前内容无要求
typedef struct {
    buzz_ogg_writer_t ogg;                            // Ogg 封装器（页缓冲调用方提供）
    buzz_opus_enc_t *enc;                             // Opus 编码器（init 创建，finish 销毁）
    int16_t frame_buf[BUZZ_OPUS_ENC_FRAME_SAMPLES];   // 跨读块攒帧缓冲
    uint32_t frame_len;                               // 攒帧缓冲已存采样数
    uint64_t granule;                                 // 下一帧 granule（48kHz 域，首帧 960）
    uint8_t pkt[BUZZ_OPUS_ENC_PKT_MAX];               // 编码输出 + 头包复用缓冲
    uint8_t last_pkt[BUZZ_OPUS_ENC_PKT_MAX];          // 悬置末包（EOS 需标在最后一帧上）
    uint32_t last_pkt_len;                            // 悬置末包长度
    uint8_t has_last;                                 // 是否存在悬置末包
    uint8_t finished;                                 // finish 已调用（幂等守卫）
    buzz_ogg_page_cb_t user_cb;                       // 调用方页回调（ctx 原样转发）
    void *user_ctx;
    buzz_rec_pipe_stats_t stats;
} buzz_rec_pipe_t;

// 会话开始：serial 为 Ogg 流序号（调用方给定，整流一致即可）；
// page_buf/page_cap 为页组装缓冲（≥ BUZZ_OGG_PAGE_BUF_MIN）；cb/ctx 为页输出口。
// 内部创建 Opus 编码器、初始化 Ogg 写入器，并立即写 OpusHead（BOS，granule=0）
// 与 OpusTags 两个独立页（RFC 7845）。失败返回负值并自清理（对象不可再用）。
int buzz_rec_pipe_init(buzz_rec_pipe_t *p, uint32_t serial, uint8_t *page_buf,
                       uint32_t page_cap, buzz_ogg_page_cb_t cb, void *ctx);

// M5.3 质量两档：显式码率版（bitrate bps，传 0 用默认 BUZZ_REC_PIPE_BITRATE）。
// 其余语义同 buzz_rec_pipe_init。
int buzz_rec_pipe_init_ex(buzz_rec_pipe_t *p, uint32_t serial, int bitrate_bps,
                          uint8_t *page_buf, uint32_t page_cap,
                          buzz_ogg_page_cb_t cb, void *ctx);

// 喂入单声道 int16 PCM 任意块：内部跨块攒满 320 采样即编码封帧，
// granule 从 960 起逐帧 +960（48kHz 域）。返回 BUZZ_OK；编码失败返回负值
// （stats.encode_errs 已计数，已封帧不受影响）。
int buzz_rec_pipe_feed(buzz_rec_pipe_t *p, const int16_t *pcm, uint32_t nsamples);

// 会话结束：丢弃不足 320 的尾采样（<20ms 无感）；悬置末包带 EOS 写入，
// 无音频帧时补空 EOS 页；销毁编码器。幂等（重复调用返回 BUZZ_OK）。
// 返回 BUZZ_OK；头两页之后的封装错误返回负值。
int buzz_rec_pipe_finish(buzz_rec_pipe_t *p);

#ifdef __cplusplus
}
#endif
