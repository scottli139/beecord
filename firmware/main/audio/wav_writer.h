// WAV（RIFF）封装写入器——录音文件落卡的封装层，硬件无关纯逻辑（只依赖 stdio，宿主可测）。
//
// 职责：生成 44 字节标准 PCM WAV 头（16 kHz / 16 bit / 单声道，对齐 BUZZLINK 录音格式），
// open 时先写占位头，write 追加 PCM 数据，close 时 fseek 回填 RIFF size 与 data size。
// 全部缓冲由本模块静态持有，零 malloc。
//
// 文件布局（小端）：
//   [0]  "RIFF" [4] chunk_size=36+data [8] "WAVE"
//   [12] "fmt " [16] fmt_size=16 [20] format=1(PCM) [22] channels=1
//   [24] sample_rate=16000 [28] byte_rate=32000 [32] block_align=2 [34] bits=16
//   [36] "data" [40] data_size
#ifndef BUZZ_WAV_WRITER_H
#define BUZZ_WAV_WRITER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "buzz_proto.h" // 错误码约定（BUZZ_OK=0，负值为错）

#ifdef __cplusplus
extern "C" {
#endif

// 文件格式常量：16 kHz / 16 bit / 单声道 PCM
#define BUZZ_WAV_SAMPLE_RATE 16000u
#define BUZZ_WAV_BITS 16u
#define BUZZ_WAV_CHANNELS 1u
#define BUZZ_WAV_BYTE_RATE (BUZZ_WAV_SAMPLE_RATE * BUZZ_WAV_CHANNELS * BUZZ_WAV_BITS / 8u) // 32000
#define BUZZ_WAV_BLOCK_ALIGN (BUZZ_WAV_CHANNELS * BUZZ_WAV_BITS / 8u)                      // 2

#define BUZZ_WAV_HEADER_LEN 44u      // 标准 PCM WAV 头长度
#define BUZZ_WAV_RIFF_OVERHEAD 36u   // chunk_size = 36 + data_size（头总长 - 8）

// 模块错误码：沿用 BUZZ_OK / BUZZ_ERR_NULL / BUZZ_ERR_SIZE（buzz_proto.h），另加 IO 错
#define BUZZ_WAV_ERR_IO (-5) // 文件 IO 失败（fopen/fwrite/fseek/fclose 出错）

// WAV 写入器：FILE* 与已写 PCM 计数，全部启动期确定，运行路径零 malloc。
typedef struct {
    FILE *fp;           // 目标文件（"wb"）；NULL = 未打开
    uint64_t data_bytes; // 已写入 PCM 字节数（回填 data size 的依据）
} buzz_wav_writer_t;

// 生成 44 字节 WAV 头到 out（cap 必须 ≥ BUZZ_WAV_HEADER_LEN）。
// data_size 为 PCM 数据总字节数（回填 RIFF chunk_size 与 data size 两个字段）。
// 返回 BUZZ_WAV_HEADER_LEN；out 为 NULL 返回 BUZZ_ERR_NULL，cap 不足返回 BUZZ_ERR_SIZE。
int buzz_wav_build_header(uint8_t *out, size_t cap, uint32_t data_size);

// 打开 path 并写入占位头（两个长度字段为 0，待 close 回填）。
// 成功返回 BUZZ_OK 且 w->fp 有效；w/path 为 NULL 返回 BUZZ_ERR_NULL，打开或写头失败返回 BUZZ_WAV_ERR_IO。
int buzz_wav_open(buzz_wav_writer_t *w, const char *path);

// 追加 len 字节 PCM。成功返回 BUZZ_OK；w/pcm 为 NULL 或未打开返回 BUZZ_ERR_NULL，
// 短写返回 BUZZ_WAV_ERR_IO（文件进入未定义状态，调用方应 close 丢弃）。
int buzz_wav_write(buzz_wav_writer_t *w, const uint8_t *pcm, uint32_t len);

// 回填 RIFF/data 长度并关闭文件；幂等（重复调用返回 BUZZ_OK）。
// fclose 失败返回 BUZZ_WAV_ERR_IO；调用后 w->fp 置 NULL，data_bytes 保留供读取统计。
int buzz_wav_close(buzz_wav_writer_t *w);

#ifdef __cplusplus
}
#endif

#endif // BUZZ_WAV_WRITER_H
