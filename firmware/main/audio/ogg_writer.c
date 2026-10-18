// Ogg 页封装写入器实现（RFC 3533）。硬件无关纯逻辑，零 malloc。
#include "ogg_writer.h"

#include <stdbool.h>
#include <string.h>

// ---------- CRC32（直连型：多项式 0x04C11DB7，初值 0，无输入/输出反射，异或出 0） ----------

static const uint32_t s_crc32_table[256] = {
    0x00000000, 0x04C11DB7, 0x09823B6E, 0x0D4326D9, 0x130476DC, 0x17C56B6B, 0x1A864DB2, 0x1E475005, 0x2608EDB8, 0x22C9F00F, 0x2F8AD6D6, 0x2B4BCB61, 0x350C9B64, 0x31CD86D3, 0x3C8EA00A, 0x384FBDBD,
    0x4C11DB70, 0x48D0C6C7, 0x4593E01E, 0x4152FDA9, 0x5F15ADAC, 0x5BD4B01B, 0x569796C2, 0x52568B75, 0x6A1936C8, 0x6ED82B7F, 0x639B0DA6, 0x675A1011, 0x791D4014, 0x7DDC5DA3, 0x709F7B7A, 0x745E66CD,
    0x9823B6E0, 0x9CE2AB57, 0x91A18D8E, 0x95609039, 0x8B27C03C, 0x8FE6DD8B, 0x82A5FB52, 0x8664E6E5, 0xBE2B5B58, 0xBAEA46EF, 0xB7A96036, 0xB3687D81, 0xAD2F2D84, 0xA9EE3033, 0xA4AD16EA, 0xA06C0B5D,
    0xD4326D90, 0xD0F37027, 0xDDB056FE, 0xD9714B49, 0xC7361B4C, 0xC3F706FB, 0xCEB42022, 0xCA753D95, 0xF23A8028, 0xF6FB9D9F, 0xFBB8BB46, 0xFF79A6F1, 0xE13EF6F4, 0xE5FFEB43, 0xE8BCCD9A, 0xEC7DD02D,
    0x34867077, 0x30476DC0, 0x3D044B19, 0x39C556AE, 0x278206AB, 0x23431B1C, 0x2E003DC5, 0x2AC12072, 0x128E9DCF, 0x164F8078, 0x1B0CA6A1, 0x1FCDBB16, 0x018AEB13, 0x054BF6A4, 0x0808D07D, 0x0CC9CDCA,
    0x7897AB07, 0x7C56B6B0, 0x71159069, 0x75D48DDE, 0x6B93DDDB, 0x6F52C06C, 0x6211E6B5, 0x66D0FB02, 0x5E9F46BF, 0x5A5E5B08, 0x571D7DD1, 0x53DC6066, 0x4D9B3063, 0x495A2DD4, 0x44190B0D, 0x40D816BA,
    0xACA5C697, 0xA864DB20, 0xA527FDF9, 0xA1E6E04E, 0xBFA1B04B, 0xBB60ADFC, 0xB6238B25, 0xB2E29692, 0x8AAD2B2F, 0x8E6C3698, 0x832F1041, 0x87EE0DF6, 0x99A95DF3, 0x9D684044, 0x902B669D, 0x94EA7B2A,
    0xE0B41DE7, 0xE4750050, 0xE9362689, 0xEDF73B3E, 0xF3B06B3B, 0xF771768C, 0xFA325055, 0xFEF34DE2, 0xC6BCF05F, 0xC27DEDE8, 0xCF3ECB31, 0xCBFFD686, 0xD5B88683, 0xD1799B34, 0xDC3ABDED, 0xD8FBA05A,
    0x690CE0EE, 0x6DCDFD59, 0x608EDB80, 0x644FC637, 0x7A089632, 0x7EC98B85, 0x738AAD5C, 0x774BB0EB, 0x4F040D56, 0x4BC510E1, 0x46863638, 0x42472B8F, 0x5C007B8A, 0x58C1663D, 0x558240E4, 0x51435D53,
    0x251D3B9E, 0x21DC2629, 0x2C9F00F0, 0x285E1D47, 0x36194D42, 0x32D850F5, 0x3F9B762C, 0x3B5A6B9B, 0x0315D626, 0x07D4CB91, 0x0A97ED48, 0x0E56F0FF, 0x1011A0FA, 0x14D0BD4D, 0x19939B94, 0x1D528623,
    0xF12F560E, 0xF5EE4BB9, 0xF8AD6D60, 0xFC6C70D7, 0xE22B20D2, 0xE6EA3D65, 0xEBA91BBC, 0xEF68060B, 0xD727BBB6, 0xD3E6A601, 0xDEA580D8, 0xDA649D6F, 0xC423CD6A, 0xC0E2D0DD, 0xCDA1F604, 0xC960EBB3,
    0xBD3E8D7E, 0xB9FF90C9, 0xB4BCB610, 0xB07DABA7, 0xAE3AFBA2, 0xAAFBE615, 0xA7B8C0CC, 0xA379DD7B, 0x9B3660C6, 0x9FF77D71, 0x92B45BA8, 0x9675461F, 0x8832161A, 0x8CF30BAD, 0x81B02D74, 0x857130C3,
    0x5D8A9099, 0x594B8D2E, 0x5408ABF7, 0x50C9B640, 0x4E8EE645, 0x4A4FFBF2, 0x470CDD2B, 0x43CDC09C, 0x7B827D21, 0x7F436096, 0x7200464F, 0x76C15BF8, 0x68860BFD, 0x6C47164A, 0x61043093, 0x65C52D24,
    0x119B4BE9, 0x155A565E, 0x18197087, 0x1CD86D30, 0x029F3D35, 0x065E2082, 0x0B1D065B, 0x0FDC1BEC, 0x3793A651, 0x3352BBE6, 0x3E119D3F, 0x3AD08088, 0x2497D08D, 0x2056CD3A, 0x2D15EBE3, 0x29D4F654,
    0xC5A92679, 0xC1683BCE, 0xCC2B1D17, 0xC8EA00A0, 0xD6AD50A5, 0xD26C4D12, 0xDF2F6BCB, 0xDBEE767C, 0xE3A1CBC1, 0xE760D676, 0xEA23F0AF, 0xEEE2ED18, 0xF0A5BD1D, 0xF464A0AA, 0xF9278673, 0xFDE69BC4,
    0x89B8FD09, 0x8D79E0BE, 0x803AC667, 0x84FBDBD0, 0x9ABC8BD5, 0x9E7D9662, 0x933EB0BB, 0x97FFAD0C, 0xAFB010B1, 0xAB710D06, 0xA6322BDF, 0xA2F33668, 0xBCB4666D, 0xB8757BDA, 0xB5365D03, 0xB1F740B4,
};

static uint32_t ogg_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0; // 直连型初值 0
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ s_crc32_table[((crc >> 24) ^ data[i]) & 0xFF];
    }
    return crc;
}

// ---------- 小端写辅助 ----------

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void write_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

// ---------- 页组装 ----------

// 组装期 payload 上限：封页时要把段表插到 payload 前面，预留 255 字节段表空间
static uint32_t payload_limit(const buzz_ogg_writer_t *w)
{
    return w->page_cap - BUZZ_OGG_PAGE_HEADER_LEN - BUZZ_OGG_MAX_SEGMENTS;
}

// 当前页能否再装一个 seg_payload 字节的段（段数与 payload 空间双重检查）
static bool page_can_fit(const buzz_ogg_writer_t *w, uint32_t seg_payload)
{
    return w->num_segments < BUZZ_OGG_MAX_SEGMENTS &&
           w->payload_len + seg_payload <= payload_limit(w);
}

// 追加一段：段值记入段表，数据拷入页缓冲 payload 区
static void page_push_segment(buzz_ogg_writer_t *w, uint8_t seg_val, const uint8_t *data)
{
    w->seg_table[w->num_segments++] = seg_val;
    if (seg_val > 0u) {
        memcpy(w->page_buf + BUZZ_OGG_PAGE_HEADER_LEN + w->payload_len, data, seg_val);
        w->payload_len += seg_val;
    }
}

// 封页并回调产出。
// mid = 本页结束时包未完（granule 置 -1，下页置续传位）；eos = 置 EOS 位；
// granule_field = 非 mid 页的 granule（常规封页传 w->granule）。
static void page_close(buzz_ogg_writer_t *w, bool mid, bool eos, uint64_t granule_field)
{
    uint8_t n = w->num_segments;
    uint32_t total = BUZZ_OGG_PAGE_HEADER_LEN + n + w->payload_len;

    // 段表插入头与 payload 之间：payload 从 27 处右移到 27+n 处
    memmove(w->page_buf + BUZZ_OGG_PAGE_HEADER_LEN + n,
            w->page_buf + BUZZ_OGG_PAGE_HEADER_LEN, w->payload_len);
    memcpy(w->page_buf + BUZZ_OGG_PAGE_HEADER_LEN, w->seg_table, n);

    uint8_t *h = w->page_buf;
    memcpy(h, "OggS", 4);
    h[4] = 0; // stream structure version
    h[5] = (uint8_t)(w->cur_header | (eos ? BUZZ_OGG_HEADER_EOS : 0u));
    write_le64(h + 6, mid ? UINT64_MAX : granule_field);
    write_le32(h + 14, w->serial);
    write_le32(h + 18, w->page_seq);
    write_le32(h + 22, 0); // CRC 计算时本字段清零
    h[26] = n;
    write_le32(h + 22, ogg_crc32(h, total));

    if (w->cb != NULL) {
        w->cb(h, total, w->ctx);
    }
    w->page_seq++;
    w->num_segments = 0;
    w->payload_len = 0;
    w->cur_header = mid ? BUZZ_OGG_HEADER_CONTINUED : 0u;
}

int buzz_ogg_writer_init(buzz_ogg_writer_t *w, uint32_t serial, uint8_t *page_buf,
                         uint32_t page_cap, buzz_ogg_page_cb_t cb, void *ctx)
{
    if (w == NULL) {
        return BUZZ_ERR_NULL;
    }
    if (page_buf == NULL || page_cap < BUZZ_OGG_PAGE_BUF_MIN) {
        memset(w, 0, sizeof(*w));
        return BUZZ_ERR_ARG;
    }
    memset(w, 0, sizeof(*w));
    w->serial = serial;
    w->page_buf = page_buf;
    w->page_cap = page_cap;
    w->cb = cb;
    w->ctx = ctx;
    return BUZZ_OK;
}

int buzz_ogg_write_packet(buzz_ogg_writer_t *w, const uint8_t *packet, uint32_t len,
                          uint64_t granule, uint8_t flags)
{
    if (w == NULL || (packet == NULL && len > 0u)) {
        return BUZZ_ERR_NULL;
    }
    if (w->finalized || (flags & ~(BUZZ_OGG_FLAG_BOS | BUZZ_OGG_FLAG_EOS)) != 0u ||
        granule < w->granule) {
        return BUZZ_ERR_ARG; // 已结束 / 保留位 / granule 回退（单调不减）
    }
    if ((flags & BUZZ_OGG_FLAG_BOS) != 0u && w->started) {
        return BUZZ_ERR_ARG; // BOS 仅允许首包
    }

    if (!w->started) {
        w->started = 1;
        if ((flags & BUZZ_OGG_FLAG_BOS) != 0u) {
            w->cur_header |= BUZZ_OGG_HEADER_BOS;
        }
    }
    if (len == 0u && w->num_segments == BUZZ_OGG_MAX_SEGMENTS) {
        page_close(w, false, false, w->granule); // 极端：空包但段表已满，先封页
    }

    const bool eos = (flags & BUZZ_OGG_FLAG_EOS) != 0u;
    uint32_t segs_needed = len / BUZZ_OGG_MAX_SEGMENT_PAYLOAD + 1u;

    if (w->num_segments + segs_needed <= BUZZ_OGG_MAX_SEGMENTS &&
        len <= payload_limit(w) - w->payload_len) {
        // 整包装入当前页：满 255 段 + 终结段（len 为 255 整数倍时终结段为 0）
        uint32_t full = len / BUZZ_OGG_MAX_SEGMENT_PAYLOAD;
        uint8_t term = (uint8_t)(len % BUZZ_OGG_MAX_SEGMENT_PAYLOAD);
        uint32_t pos = 0;
        for (uint32_t i = 0; i < full; i++) {
            page_push_segment(w, BUZZ_OGG_MAX_SEGMENT_PAYLOAD, packet + pos);
            pos += BUZZ_OGG_MAX_SEGMENT_PAYLOAD;
        }
        page_push_segment(w, term, packet + pos); // term==0 即"整除补 0 段"
        w->granule = granule;
        if (eos) {
            page_close(w, false, true, w->granule);
            w->finalized = 1;
        }
        return BUZZ_OK;
    }

    // 整包装不下：先在包边界封当前页，再逐段跨页装（页满封页、下页续传）
    if (w->num_segments > 0u) {
        page_close(w, false, false, w->granule);
    }
    uint32_t pos = 0;
    uint32_t remaining = len;
    while (remaining > 0u) {
        if (remaining > BUZZ_OGG_MAX_SEGMENT_PAYLOAD) {
            if (page_can_fit(w, BUZZ_OGG_MAX_SEGMENT_PAYLOAD)) {
                page_push_segment(w, BUZZ_OGG_MAX_SEGMENT_PAYLOAD, packet + pos);
                pos += BUZZ_OGG_MAX_SEGMENT_PAYLOAD;
                remaining -= BUZZ_OGG_MAX_SEGMENT_PAYLOAD;
            } else {
                page_close(w, true, false, 0);
            }
        } else if (remaining == BUZZ_OGG_MAX_SEGMENT_PAYLOAD) {
            if (page_can_fit(w, BUZZ_OGG_MAX_SEGMENT_PAYLOAD)) {
                page_push_segment(w, BUZZ_OGG_MAX_SEGMENT_PAYLOAD, packet + pos);
                pos += BUZZ_OGG_MAX_SEGMENT_PAYLOAD;
                remaining = 0;
            } else {
                page_close(w, true, false, 0);
            }
        } else { // remaining < 255：终结段
            if (page_can_fit(w, remaining)) {
                page_push_segment(w, (uint8_t)remaining, packet + pos);
                remaining = 0;
            } else {
                page_close(w, true, false, 0);
            }
        }
    }
    if (len > 0u && len % BUZZ_OGG_MAX_SEGMENT_PAYLOAD == 0u) {
        // 末段是 255（整除情形）：补 0 终结段；本页没空间则封页续传、0 段落到下页
        if (!page_can_fit(w, 0u)) {
            page_close(w, true, false, 0);
        }
        page_push_segment(w, 0u, NULL);
    }
    w->granule = granule;
    if (eos) {
        page_close(w, false, true, w->granule);
        w->finalized = 1;
    }
    return BUZZ_OK;
}

int buzz_ogg_flush_page(buzz_ogg_writer_t *w)
{
    if (w == NULL) {
        return BUZZ_ERR_NULL;
    }
    if (w->num_segments > 0u) {
        page_close(w, false, false, w->granule);
    }
    return BUZZ_OK;
}

int buzz_ogg_finalize(buzz_ogg_writer_t *w, uint64_t granule)
{
    if (w == NULL) {
        return BUZZ_ERR_NULL;
    }
    if (w->finalized) {
        return BUZZ_OK; // 幂等：末包可能已带 EOS 写入
    }
    if (granule < w->granule) {
        return BUZZ_ERR_ARG;
    }
    if (w->num_segments > 0u) {
        page_close(w, false, false, w->granule);
    }
    page_close(w, false, true, granule); // 空 EOS 末页，granule 取末包 granule
    w->finalized = 1;
    return BUZZ_OK;
}

int buzz_ogg_opus_head(uint8_t *out, size_t cap)
{
    if (out == NULL) {
        return BUZZ_ERR_NULL;
    }
    if (cap < BUZZ_OGG_OPUS_HEAD_LEN) {
        return BUZZ_ERR_SIZE;
    }
    memcpy(out, "OpusHead", 8);
    out[8] = 1; // OpusHead version
    out[9] = BUZZ_OGG_OPUS_CHANNELS;
    write_le16(out + 10, (uint16_t)BUZZ_OGG_OPUS_PRESKIP);
    write_le32(out + 12, BUZZ_OGG_OPUS_INPUT_RATE);
    write_le16(out + 16, 0); // output gain
    out[18] = 0;             // mapping family（单声道免映射）
    return (int)BUZZ_OGG_OPUS_HEAD_LEN;
}

int buzz_ogg_opus_tags(uint8_t *out, size_t cap, const char *vendor)
{
    if (out == NULL) {
        return BUZZ_ERR_NULL;
    }
    if (vendor == NULL) {
        vendor = "beecord";
    }
    size_t vlen = strlen(vendor);
    size_t total = 8u + 4u + vlen + 4u;
    if (cap < total) {
        return BUZZ_ERR_SIZE;
    }
    memcpy(out, "OpusTags", 8);
    write_le32(out + 8, (uint32_t)vlen);
    memcpy(out + 12, vendor, vlen);
    write_le32(out + 12 + vlen, 0); // 0 条用户注释
    return (int)total;
}
