// 硬件无关录音管线实现（M3）。帧切片 → Opus 编码 → Ogg 封装的会话编排，
// 全部缓冲内嵌或调用方提供，采集循环零 malloc。
#include "rec_pipe.h"

#include <string.h>

// 内部页回调：先累计输出字节统计，再原样转发调用方回调
static void pipe_page_cb(const uint8_t *page, uint32_t page_len, void *ctx)
{
    buzz_rec_pipe_t *p = (buzz_rec_pipe_t *)ctx;
    p->stats.bytes_out += page_len;
    if (p->user_cb != NULL) {
        p->user_cb(page, page_len, p->user_ctx);
    }
}

int buzz_rec_pipe_init(buzz_rec_pipe_t *p, uint32_t serial, uint8_t *page_buf,
                       uint32_t page_cap, buzz_ogg_page_cb_t cb, void *ctx)
{
    return buzz_rec_pipe_init_ex(p, serial, 0, page_buf, page_cap, cb, ctx);
}

int buzz_rec_pipe_init_ex(buzz_rec_pipe_t *p, uint32_t serial, int bitrate_bps,
                          uint8_t *page_buf, uint32_t page_cap,
                          buzz_ogg_page_cb_t cb, void *ctx)
{
    if (p == NULL) {
        return BUZZ_ERR_NULL;
    }
    memset(p, 0, sizeof(*p));
    p->user_cb = cb;
    p->user_ctx = ctx;

    // M5.3 质量两档：bitrate_bps>0 用显式码率，否则默认 44kbps
    const int bitrate = bitrate_bps > 0 ? bitrate_bps : BUZZ_REC_PIPE_BITRATE;
    p->enc = buzz_opus_enc_create(bitrate, BUZZ_REC_PIPE_COMPLEXITY);
    if (p->enc == NULL) {
        return BUZZ_ERR_ARG; // 编码器创建失败（参数 / libopus / 内存）
    }
    int ret = buzz_ogg_writer_init(&p->ogg, serial, page_buf, page_cap, pipe_page_cb, p);
    if (ret != BUZZ_OK) {
        buzz_opus_enc_destroy(p->enc);
        p->enc = NULL;
        return ret;
    }
    p->granule = BUZZ_OPUS_ENC_GRANULE_STEP; // 首音频包 granule=960

    // RFC 7845 头两页独立：OpusHead（BOS 独占）→ flush → OpusTags → flush
    int head_len = buzz_ogg_opus_head(p->pkt, sizeof(p->pkt));
    if (head_len < 0 ||
        buzz_ogg_write_packet(&p->ogg, p->pkt, (uint32_t)head_len, 0u,
                              BUZZ_OGG_FLAG_BOS) != BUZZ_OK ||
        buzz_ogg_flush_page(&p->ogg) != BUZZ_OK) {
        goto fail;
    }
    int tags_len = buzz_ogg_opus_tags(p->pkt, sizeof(p->pkt), NULL);
    if (tags_len < 0 ||
        buzz_ogg_write_packet(&p->ogg, p->pkt, (uint32_t)tags_len, 0u, 0u) != BUZZ_OK ||
        buzz_ogg_flush_page(&p->ogg) != BUZZ_OK) {
        goto fail;
    }
    return BUZZ_OK;

fail:
    buzz_opus_enc_destroy(p->enc);
    p->enc = NULL;
    return BUZZ_ERR_ARG;
}

int buzz_rec_pipe_feed(buzz_rec_pipe_t *p, const int16_t *pcm, uint32_t nsamples)
{
    if (p == NULL || (pcm == NULL && nsamples > 0u)) {
        return BUZZ_ERR_NULL;
    }
    if (p->finished || p->enc == NULL) {
        return BUZZ_ERR_ARG;
    }
    p->stats.samples_in += nsamples;

    uint32_t pos = 0;
    while (pos < nsamples) {
        // 攒帧：从输入块切片补齐 frame_buf（跨读块余量留存）
        uint32_t need = (uint32_t)BUZZ_OPUS_ENC_FRAME_SAMPLES - p->frame_len;
        uint32_t take = nsamples - pos;
        if (take > need) {
            take = need;
        }
        memcpy(p->frame_buf + p->frame_len, pcm + pos, take * sizeof(int16_t));
        p->frame_len += take;
        pos += take;
        if (p->frame_len < (uint32_t)BUZZ_OPUS_ENC_FRAME_SAMPLES) {
            break;
        }

        // 满帧：编码。EOS 需标在最后一帧上，故末包悬置一拍，新帧到位先写旧包
        const int n = buzz_opus_enc_encode(p->enc, p->frame_buf, p->pkt,
                                           (uint32_t)sizeof(p->pkt));
        p->frame_len = 0;
        if (n < 0) {
            p->stats.encode_errs++;
            return n;
        }
        if (p->has_last) {
            const int wret = buzz_ogg_write_packet(&p->ogg, p->last_pkt,
                                                   p->last_pkt_len, p->granule, 0u);
            if (wret != BUZZ_OK) {
                return wret;
            }
            p->stats.frames++;
            p->granule += BUZZ_OPUS_ENC_GRANULE_STEP;
        }
        memcpy(p->last_pkt, p->pkt, (size_t)n);
        p->last_pkt_len = (uint32_t)n;
        p->has_last = 1;
    }
    return BUZZ_OK;
}

int buzz_rec_pipe_finish(buzz_rec_pipe_t *p)
{
    if (p == NULL) {
        return BUZZ_ERR_NULL;
    }
    if (p->finished) {
        return BUZZ_OK; // 幂等
    }
    p->finished = 1;

    int ret = BUZZ_OK;
    if (p->has_last) {
        // 最后一帧带 EOS 写入，EOS 页随该页封出
        ret = buzz_ogg_write_packet(&p->ogg, p->last_pkt, p->last_pkt_len, p->granule,
                                    BUZZ_OGG_FLAG_EOS);
        if (ret == BUZZ_OK) {
            p->stats.frames++;
        }
    } else {
        // 无音频帧：头两页之后补空 EOS 末页，granule 取末包（头页）granule=0
        ret = buzz_ogg_finalize(&p->ogg, p->ogg.granule);
    }
    if (p->enc != NULL) {
        buzz_opus_enc_destroy(p->enc);
        p->enc = NULL;
    }
    return ret;
}
