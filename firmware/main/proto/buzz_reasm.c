// BuzzLink 协议层 —— FRAG 分片重组实现。硬件无关，可宿主单测。
#include "buzz_reasm.h"

#include <string.h>

// 位图操作：bit i = 片 i 是否已收
static bool bitmap_get(const buzz_reasm_t *reasm, uint16_t idx)
{
    return (reasm->bitmap[idx >> 3] & (uint8_t)(1u << (idx & 7u))) != 0u;
}

static void bitmap_set(buzz_reasm_t *reasm, uint16_t idx)
{
    reasm->bitmap[idx >> 3] |= (uint8_t)(1u << (idx & 7u));
}

void buzz_reasm_init(buzz_reasm_t *reasm, uint8_t *buf, uint32_t buf_cap,
                     uint8_t *bitmap, uint32_t bitmap_bits)
{
    if (reasm == NULL) {
        return;
    }
    // 先清控制字段，再绑定缓冲：buf/bitmap 为 NULL 时等价于禁用重组
    memset(reasm, 0, sizeof(*reasm));
    reasm->buf = buf;
    reasm->buf_cap = buf_cap;
    reasm->bitmap = bitmap;
    reasm->bitmap_bits = bitmap_bits;
}

void buzz_reasm_reset(buzz_reasm_t *reasm)
{
    if (reasm == NULL) {
        return;
    }
    reasm->active = 0;
    reasm->received = 0;
    reasm->stashed = 0;
}

// 落位一片数据：越上限（64KB 硬顶 / 调用方缓冲）返回对应错误并复位。
static int frag_place(buzz_reasm_t *reasm, uint16_t idx, uint32_t offset,
                      const uint8_t *data, uint16_t len, bool is_last)
{
    uint32_t end = offset + (uint32_t)len;
    if (end > BUZZ_MSG_MAX_LEN) {
        buzz_reasm_reset(reasm);
        return BUZZ_ERR_PROTO; // 超协议 64 KB 硬顶
    }
    if (end > reasm->buf_cap) {
        buzz_reasm_reset(reasm);
        return BUZZ_ERR_SIZE; // 超调用方缓冲
    }
    if (len > 0) {
        memcpy(reasm->buf + offset, data, len);
    }
    bitmap_set(reasm, idx);
    reasm->received++;
    if (is_last) {
        reasm->last_len = len;
    }
    return BUZZ_OK;
}

// 尝试完成：片齐且暂存已消化时交付消息视图。
static bool try_finish(buzz_reasm_t *reasm, const buzz_frame_t *frame,
                       const buzz_frame_t **out)
{
    if (reasm->received != (uint32_t)reasm->frag_count || reasm->stashed != 0u) {
        return false;
    }
    uint32_t total = (uint32_t)(reasm->frag_count - 1u) * (uint32_t)reasm->stride +
                     (uint32_t)reasm->last_len;
    reasm->msg.version = frame->version;
    reasm->msg.type = reasm->type;
    reasm->msg.session = reasm->session;
    reasm->msg.flags = 0; // FRAG 已消解
    reasm->msg.channel = reasm->channel;
    reasm->msg.length = total;
    reasm->msg.payload = reasm->buf;
    *out = &reasm->msg;
    buzz_reasm_reset(reasm);
    return true;
}

int buzz_reasm_feed(buzz_reasm_t *reasm, const buzz_frame_t *frame, const buzz_frame_t **out)
{
    if (reasm == NULL || frame == NULL || out == NULL) {
        return BUZZ_ERR_NULL;
    }
    *out = NULL;

    if ((frame->flags & BUZZ_FLAG_FRAG) == 0u) {
        *out = frame; // 非 FRAG 直接透传
        return BUZZ_OK;
    }
    uint16_t frag_index, frag_count;
    const uint8_t *data = NULL;
    uint16_t data_len = 0;
    if (buzz_frag_parse(frame, &frag_index, &frag_count, &data, &data_len) != BUZZ_OK) {
        buzz_reasm_reset(reasm);
        return BUZZ_ERR_PROTO;
    }
    if (frag_count < 1u || frag_index >= frag_count) {
        buzz_reasm_reset(reasm);
        return BUZZ_ERR_PROTO;
    }

    if (reasm->active == 0u) {
        // 新消息起步检查：位图容量 + 上限上界（stride 未定的最坏情况 len×count）
        if ((uint32_t)frag_count > reasm->bitmap_bits) {
            return BUZZ_ERR_SIZE;
        }
        if ((uint32_t)data_len * (uint32_t)frag_count > BUZZ_MSG_MAX_LEN) {
            return BUZZ_ERR_PROTO;
        }
        if (reasm->buf == NULL || reasm->bitmap == NULL) {
            return BUZZ_ERR_SIZE;
        }
        memset(reasm->bitmap, 0, ((size_t)frag_count + 7u) / 8u);
        reasm->active = 1;
        reasm->type = frame->type;
        reasm->session = frame->session;
        reasm->channel = frame->channel;
        reasm->stride = 0;
        reasm->frag_count = frag_count;
        reasm->last_len = 0;
        reasm->received = 0;
        reasm->stashed = 0;
    } else if (frame->type != reasm->type || frame->session != reasm->session ||
               frame->channel != reasm->channel || frag_count != reasm->frag_count) {
        // 与进行中消息不属于同一条：放弃并报错（调用方应以超时/ERROR 善后）
        buzz_reasm_reset(reasm);
        return BUZZ_ERR_PROTO;
    }

    if (bitmap_get(reasm, frag_index)) {
        return BUZZ_OK; // 重复片：幂等忽略
    }

    const bool is_last = (frag_index == reasm->frag_count - 1u);
    int rc;
    if (is_last && reasm->stride == 0u && frag_index > 0u) {
        // 末片早于任何非末片到达：仅暂存数据（不计收到数），待 stride 确定后落位
        if (data_len > 0) {
            memcpy(reasm->stash, data, data_len);
        }
        reasm->stash_len = data_len;
        reasm->stashed = 1;
    } else if (is_last) {
        if (reasm->stride != 0u && data_len > reasm->stride) {
            buzz_reasm_reset(reasm);
            return BUZZ_ERR_PROTO; // 末片长于非末片片长，矛盾
        }
        uint32_t offset = (reasm->stride == 0u) ? 0u
                                                : (uint32_t)frag_index * (uint32_t)reasm->stride;
        rc = frag_place(reasm, frag_index, offset, data, data_len, true);
        if (rc != BUZZ_OK) {
            return rc;
        }
    } else {
        if (reasm->stride == 0u) {
            reasm->stride = data_len; // 首片非末片定 stride
        } else if (data_len != reasm->stride) {
            buzz_reasm_reset(reasm);
            return BUZZ_ERR_PROTO; // 非末片片长不一致
        }
        rc = frag_place(reasm, frag_index, (uint32_t)frag_index * (uint32_t)reasm->stride,
                        data, data_len, false);
        if (rc != BUZZ_OK) {
            return rc;
        }
        if (reasm->stashed != 0u) {
            // stride 刚确定：补落位暂存的末片
            uint32_t last_off = (uint32_t)(reasm->frag_count - 1u) * (uint32_t)reasm->stride;
            rc = frag_place(reasm, reasm->frag_count - 1u, last_off, reasm->stash,
                            reasm->stash_len, true);
            reasm->stashed = 0;
            if (rc != BUZZ_OK) {
                return rc;
            }
        }
    }

    if (!try_finish(reasm, frame, out)) {
        *out = NULL;
    }
    return BUZZ_OK;
}
