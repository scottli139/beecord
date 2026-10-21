// Beecord 录音 journal 纯逻辑实现（rec_journal.c，M5.3）。
#include "rec_journal.h"

#include <stddef.h>
#include <string.h>

#include "crc16.h"

// 字节拼装助手（小端）
static void put_u16(uint8_t **p, uint16_t v)
{
    *(*p)++ = (uint8_t)(v & 0xFFu);
    *(*p)++ = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t **p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        *(*p)++ = (uint8_t)(v >> (8 * i));
    }
}

static void put_u64(uint8_t **p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        *(*p)++ = (uint8_t)(v >> (8 * i));
    }
}

static uint16_t get_u16(const uint8_t **p)
{
    const uint8_t lo = *(*p)++;
    const uint8_t hi = *(*p)++;
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

static uint32_t get_u32(const uint8_t **p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= (uint32_t)(*(*p)++) << (8 * i);
    }
    return v;
}

static uint64_t get_u64(const uint8_t **p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)(*(*p)++) << (8 * i);
    }
    return v;
}

int rec_journal_encode(const rec_journal_t *j, uint8_t *buf, uint32_t cap)
{
    if (j == NULL || buf == NULL || cap < REC_JOURNAL_ENCODED_SIZE) {
        return -1;
    }
    uint8_t *p = buf;
    put_u32(&p, REC_JOURNAL_MAGIC);
    put_u16(&p, (uint16_t)REC_JOURNAL_VER);
    put_u16(&p, (uint16_t)REC_JOURNAL_ENCODED_SIZE);
    // 文件名字段定长写（含 NUL 填充，避免脏缓冲残留）
    for (uint32_t i = 0; i < REC_JOURNAL_FILE_MAX; i++) {
        *p++ = (uint8_t)j->file[i];
    }
    *p++ = j->fmt;
    *p++ = j->reserved[0];
    *p++ = j->reserved[1];
    *p++ = j->reserved[2];
    put_u64(&p, j->bytes);
    put_u32(&p, j->elapsed_ms);
    put_u32(&p, j->marks);
    for (uint32_t i = 0; i < REC_JOURNAL_MARKS_MAX; i++) {
        put_u32(&p, i < j->marks ? j->mark_ms[i] : 0u);
    }
    // CRC16 覆盖 magic 至末尾前 2 字节
    const uint32_t body_len = (uint32_t)(p - buf);
    const uint16_t crc = buzz_crc16(buf, body_len);
    put_u16(&p, crc);
    return (int)(p - buf);
}

int rec_journal_decode(const uint8_t *buf, uint32_t len, rec_journal_t *out)
{
    if (buf == NULL || out == NULL || len < REC_JOURNAL_ENCODED_SIZE) {
        return -1;
    }
    const uint8_t *p = buf;
    if (get_u32(&p) != REC_JOURNAL_MAGIC) {
        return -1;
    }
    if (get_u16(&p) != REC_JOURNAL_VER) {
        return -1;
    }
    if (get_u16(&p) != (uint16_t)REC_JOURNAL_ENCODED_SIZE) {
        return -1;
    }
    for (uint32_t i = 0; i < REC_JOURNAL_FILE_MAX; i++) {
        out->file[i] = (char)(*p++);
    }
    out->file[REC_JOURNAL_FILE_MAX - 1] = '\0'; // 防御：强制收尾
    out->fmt = *p++;
    out->reserved[0] = *p++;
    out->reserved[1] = *p++;
    out->reserved[2] = *p++;
    out->bytes = get_u64(&p);
    out->elapsed_ms = get_u32(&p);
    out->marks = get_u32(&p);
    for (uint32_t i = 0; i < REC_JOURNAL_MARKS_MAX; i++) {
        out->mark_ms[i] = get_u32(&p);
    }
    const uint16_t crc_stored = get_u16(&p);
    const uint16_t crc_calc = buzz_crc16(buf, (uint32_t)(p - buf) - 2u);
    if (crc_stored != crc_calc) {
        return -1;
    }
    if (out->marks > REC_JOURNAL_MARKS_MAX) {
        return -1;
    }
    if (out->fmt > 1u) {
        return -1;
    }
    return 0;
}

bool rec_journal_plan_recovery(const rec_journal_t *j, uint64_t actual_size,
                               uint64_t *out_truncate_to)
{
    if (j == NULL || out_truncate_to == NULL) {
        return false;
    }
    if (actual_size == 0u) {
        return false; // 文件不存在：无可恢复对象（仅 toast）
    }
    // 截断目标：已确认字节；但不得小于 0（journal 由会话期写入，恒 < 实际
    // 或相等——断电时文件可能已含部分未确认尾页）。文件更小时以实际为准。
    *out_truncate_to = (actual_size > j->bytes) ? j->bytes : actual_size;
    return true;
}

void rec_journal_wav_patch_header(uint8_t *hdr44, uint64_t data_bytes)
{
    if (hdr44 == NULL) {
        return;
    }
    const uint32_t riff_size = (uint32_t)data_bytes + 36u; // 36 = 44 - 8
    const uint32_t data_size = (uint32_t)data_bytes;
    hdr44[4] = (uint8_t)(riff_size & 0xFFu);
    hdr44[5] = (uint8_t)((riff_size >> 8) & 0xFFu);
    hdr44[6] = (uint8_t)((riff_size >> 16) & 0xFFu);
    hdr44[7] = (uint8_t)((riff_size >> 24) & 0xFFu);
    hdr44[40] = (uint8_t)(data_size & 0xFFu);
    hdr44[41] = (uint8_t)((data_size >> 8) & 0xFFu);
    hdr44[42] = (uint8_t)((data_size >> 16) & 0xFFu);
    hdr44[43] = (uint8_t)((data_size >> 24) & 0xFFu);
}
