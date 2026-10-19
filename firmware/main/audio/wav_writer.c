// WAV（RIFF）封装写入器实现。硬件无关纯逻辑，零 malloc。
#include "wav_writer.h"

#include <string.h>

// ---------- 小端写辅助 ----------

static void wav_write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wav_write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int buzz_wav_build_header(uint8_t *out, size_t cap, uint32_t data_size)
{
    if (out == NULL) {
        return BUZZ_ERR_NULL;
    }
    if (cap < BUZZ_WAV_HEADER_LEN) {
        return BUZZ_ERR_SIZE;
    }
    memcpy(out + 0, "RIFF", 4);
    wav_write_le32(out + 4, BUZZ_WAV_RIFF_OVERHEAD + data_size);
    memcpy(out + 8, "WAVE", 4);
    memcpy(out + 12, "fmt ", 4);
    wav_write_le32(out + 16, 16u); // PCM fmt 块长度
    wav_write_le16(out + 20, 1u);  // audio format = PCM
    wav_write_le16(out + 22, (uint16_t)BUZZ_WAV_CHANNELS);
    wav_write_le32(out + 24, BUZZ_WAV_SAMPLE_RATE);
    wav_write_le32(out + 28, BUZZ_WAV_BYTE_RATE);
    wav_write_le16(out + 32, (uint16_t)BUZZ_WAV_BLOCK_ALIGN);
    wav_write_le16(out + 34, (uint16_t)BUZZ_WAV_BITS);
    memcpy(out + 36, "data", 4);
    wav_write_le32(out + 40, data_size);
    return (int)BUZZ_WAV_HEADER_LEN;
}

int buzz_wav_open(buzz_wav_writer_t *w, const char *path)
{
    if (w == NULL || path == NULL) {
        return BUZZ_ERR_NULL;
    }
    memset(w, 0, sizeof(*w));
    w->fp = fopen(path, "wb");
    if (w->fp == NULL) {
        return BUZZ_WAV_ERR_IO;
    }
    uint8_t header[BUZZ_WAV_HEADER_LEN];
    buzz_wav_build_header(header, sizeof(header), 0u);
    if (fwrite(header, 1, sizeof(header), w->fp) != sizeof(header)) {
        fclose(w->fp);
        w->fp = NULL;
        return BUZZ_WAV_ERR_IO;
    }
    return BUZZ_OK;
}

int buzz_wav_write(buzz_wav_writer_t *w, const uint8_t *pcm, uint32_t len)
{
    if (w == NULL || (pcm == NULL && len > 0u)) {
        return BUZZ_ERR_NULL;
    }
    if (w->fp == NULL) {
        return BUZZ_ERR_NULL; // 未打开（或已 close）
    }
    if (len > 0u && fwrite(pcm, 1, len, w->fp) != len) {
        return BUZZ_WAV_ERR_IO;
    }
    w->data_bytes += len;
    return BUZZ_OK;
}

int buzz_wav_close(buzz_wav_writer_t *w)
{
    if (w == NULL) {
        return BUZZ_ERR_NULL;
    }
    if (w->fp == NULL) {
        return BUZZ_OK; // 幂等：未打开或已关闭
    }
    uint8_t header[BUZZ_WAV_HEADER_LEN];
    const uint32_t data_size =
        w->data_bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)w->data_bytes;
    buzz_wav_build_header(header, sizeof(header), data_size);
    int ret = BUZZ_OK;
    if (fseek(w->fp, 0, SEEK_SET) != 0 ||
        fwrite(header, 1, sizeof(header), w->fp) != sizeof(header)) {
        ret = BUZZ_WAV_ERR_IO; // 回填失败：文件头长度字段仍为 0
    }
    if (fclose(w->fp) != 0) {
        ret = BUZZ_WAV_ERR_IO;
    }
    w->fp = NULL;
    return ret;
}
