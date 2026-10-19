// Opus 编码器薄包装（M3）：把 libopus 收敛为 create/encode/destroy 单一接口。
//
// 头文件只暴露 opaque 句柄，不 include opus.h（opus 类型只在 .c 内可见）。
// 同一套源码两处编译：目标侧链接 micro-opus 组件、宿主侧链接系统 libopus。
// 编码参数固定 16kHz 单声道（BuzzLink 录音格式）、20ms 帧。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 帧参数：16kHz × 20ms = 320 采样；granule 步进为其 48kHz 域等值 960
#define BUZZ_OPUS_ENC_SAMPLE_RATE 16000
#define BUZZ_OPUS_ENC_FRAME_SAMPLES 320
#define BUZZ_OPUS_ENC_GRANULE_STEP 960u // 48kHz 域，供调用方累计 granule

// 单帧输出缓冲上限：RFC 6716 规定 20ms 帧最大 1275 字节，取整留余量
#define BUZZ_OPUS_ENC_PKT_MAX 1500u

typedef struct buzz_opus_enc buzz_opus_enc_t;

// 创建编码器：bitrate（bps，如 44000）、complexity（0~10）。
// 内部固定 OPUS_APPLICATION_AUDIO（SILK/hybrid 自适应，适合会议+环境声）。
// 失败（参数非法 / libopus 报错 / 内存不足）返回 NULL。
buzz_opus_enc_t *buzz_opus_enc_create(int bitrate, int complexity);

// 编码一帧：pcm 恰 BUZZ_OPUS_ENC_FRAME_SAMPLES 个 int16 单声道采样，
// out 容量 ≥ BUZZ_OPUS_ENC_PKT_MAX。返回包长（>0）；参数非法 / 编码失败返回负值。
int buzz_opus_enc_encode(buzz_opus_enc_t *enc, const int16_t *pcm, uint8_t *out,
                         uint32_t cap);

// 销毁编码器（NULL 安全）。
void buzz_opus_enc_destroy(buzz_opus_enc_t *enc);

#ifdef __cplusplus
}
#endif
