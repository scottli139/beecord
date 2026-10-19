// Opus 编码器薄包装实现（M3）。唯一允许 include opus.h 的模块：
// 目标侧由 micro-opus 组件提供头文件，宿主侧由系统 libopus（pkg-config）提供。
#include "opus_enc.h"

#include <stddef.h>
#include <stdlib.h>

#include <opus.h>

#include "buzz_proto.h" // 错误码约定（负值为错）

struct buzz_opus_enc {
    OpusEncoder *enc;
};

buzz_opus_enc_t *buzz_opus_enc_create(int bitrate, int complexity)
{
    int err = OPUS_OK;
    OpusEncoder *e =
        opus_encoder_create(BUZZ_OPUS_ENC_SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO, &err);
    if (e == NULL || err != OPUS_OK) {
        if (e != NULL) {
            opus_encoder_destroy(e);
        }
        return NULL;
    }
    if (opus_encoder_ctl(e, OPUS_SET_BITRATE(bitrate)) != OPUS_OK ||
        opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(complexity)) != OPUS_OK) {
        opus_encoder_destroy(e);
        return NULL;
    }
    buzz_opus_enc_t *h = (buzz_opus_enc_t *)malloc(sizeof(*h));
    if (h == NULL) {
        opus_encoder_destroy(e);
        return NULL;
    }
    h->enc = e;
    return h;
}

int buzz_opus_enc_encode(buzz_opus_enc_t *enc, const int16_t *pcm, uint8_t *out,
                         uint32_t cap)
{
    if (enc == NULL || pcm == NULL || out == NULL) {
        return BUZZ_ERR_NULL;
    }
    if (cap < BUZZ_OPUS_ENC_PKT_MAX) {
        return BUZZ_ERR_SIZE;
    }
    const int n = opus_encode(enc->enc, pcm, BUZZ_OPUS_ENC_FRAME_SAMPLES,
                              out, (opus_int32)cap);
    if (n < 0) {
        return BUZZ_ERR_ARG; // libopus 内部错误（如内存/状态异常），无更细粒度必要
    }
    return n;
}

void buzz_opus_enc_destroy(buzz_opus_enc_t *enc)
{
    if (enc == NULL) {
        return;
    }
    opus_encoder_destroy(enc->enc);
    free(enc);
}
