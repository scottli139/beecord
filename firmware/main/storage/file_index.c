// 录音文件索引与元数据（M4.3）实现。硬件无关、零 malloc（全部静态缓冲）、
// 单实例。解析规则与容错约定见 file_index.h 头注释。
#include "file_index.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h> // strcasecmp

#include "crc16.h"

// ---------- 静态状态（单实例，零 malloc） ----------

static buzz_file_io_t s_io;
static bool s_ready;

// id 槽位：list() 重建索引时先收集再排序，元数据仅对切片条目填齐。
static buzz_file_entry_t s_ids[BUZZ_INDEX_MAX_FILES];

// 解析复用缓冲（静态避免占调用栈；服务任务单上下文串行使用）。
// 窗口宏先于缓冲定义，供其定长。
#define BUZZ_WAV_SCAN_CAP 1024u  // WAV fmt/data chunk 扫描窗口（M3 头固定 44B，余量为容错）
#define BUZZ_OGG_SCAN_MAX 65536u // OGG 尾扫窗口上限（读文件尾 64 KB）
#define BUZZ_OGG_GRANULE_HZ 48000u // RFC 7845：Opus granule 为 48 kHz 域采样数

static uint8_t s_chunk[4096];           // OGG 尾扫块
static uint8_t s_page_hdr[27u + 255u]; // OGG 候选页头 + 段表
static uint8_t s_io_buf[BUZZ_WAV_SCAN_CAP]; // 流式 CRC 块 / WAV 头扫描窗口

// ---------- 小端读取 ----------

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd64le(const uint8_t *p)
{
    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

// ---------- id 过滤 ----------

static bool has_ext(const char *name, const char *ext)
{
    const size_t nlen = strlen(name);
    const size_t elen = strlen(ext);
    if (nlen < 4u + elen || nlen > BUZZ_FILE_ID_MAX_LEN) {
        return false;
    }
    // 扩展名比较大小写不敏感（FAT 文件名大小写不保证一致）
    return strcasecmp(name + nlen - elen, ext) == 0;
}

bool buzz_file_index_id_match(const char *name)
{
    if (name == NULL) {
        return false;
    }
    return strncmp(name, "REC_", 4u) == 0 &&
           (has_ext(name, ".WAV") || has_ext(name, ".OGG"));
}

static bool is_ogg_name(const char *name)
{
    return has_ext(name, ".OGG");
}

// ---------- WAV（RIFF）解析 ----------

// 解析 fmt/data chunk：byteRate 缺省 0（durationMs 归 0）；sampleRate/channels
// 取自 fmt chunk（缺省 0）。RIFF/WAVE 魔数缺失或 chunk 走不完时按损坏处理，
// 返回仍"成功"（duration 0）——crc16/size 不受影响，容错不报错。
static void wav_parse(const uint8_t *buf, uint32_t len, uint32_t *byte_rate,
                      uint32_t *data_size, uint32_t *rate, uint32_t *ch)
{
    *byte_rate = 0;
    *data_size = 0;
    *rate = 0;
    *ch = 0;
    if (len < 12u || memcmp(buf, "RIFF", 4u) != 0 || memcmp(buf + 8, "WAVE", 4u) != 0) {
        return;
    }
    uint32_t off = 12u;
    bool have_fmt = false;
    bool have_data = false;
    while (off + 8u <= len && !(have_fmt && have_data)) {
        const uint8_t *cid = buf + off;
        const uint32_t csize = rd32le(buf + off + 4u);
        const uint32_t avail = len - off - 8u; // chunk 数据在本窗口内可读字节
        if (memcmp(cid, "fmt ", 4u) == 0 && csize >= 16u && avail >= 16u) {
            *ch = rd16le(buf + off + 8u + 2u);
            *rate = rd32le(buf + off + 8u + 4u);
            *byte_rate = rd32le(buf + off + 8u + 8u);
            have_fmt = true;
        } else if (memcmp(cid, "data", 4u) == 0) {
            *data_size = csize; // RIFF 声明值即可，不必读穿数据体
            have_data = true;
        }
        // 奇数长 chunk 有 1 字节填充；步进到下一 chunk（越界即走不完，容错退出）
        const uint64_t next = (uint64_t)off + 8u + csize + (csize & 1u);
        if (next > len || next <= off) {
            break;
        }
        off = (uint32_t)next;
    }
}

// 计算 WAV durationMs 并给出 fmt 字段；fh 位于文件头（read_at 无游标，传入仅为
// 读源）。fsize == 0 时全部输出 0。
static void wav_meta(void *fh, uint32_t fsize, uint32_t *rate, uint32_t *ch,
                     uint32_t *duration_ms)
{
    *rate = 0;
    *ch = 0;
    *duration_ms = 0;
    if (fsize == 0u) {
        return;
    }
    const uint32_t want = fsize < BUZZ_WAV_SCAN_CAP ? fsize : BUZZ_WAV_SCAN_CAP;
    const long n = s_io.read_at(s_io.ctx, fh, 0u, s_io_buf, want);
    if (n < (long)want) {
        return; // 头读不全：按损坏处理（duration 0）
    }
    uint32_t byte_rate;
    uint32_t data_size;
    wav_parse(s_io_buf, (uint32_t)n, &byte_rate, &data_size, rate, ch);
    if (byte_rate > 0u) {
        *duration_ms = (uint32_t)(((uint64_t)data_size * 1000u) / byte_rate);
    }
}

// ---------- OGG 尾扫解析 ----------

// 从文件尾向前按块扫描，找最后一个 OggS 页（version=0 且 页头+段表+段数据
// 恰好到 EOF —— 即"最后一页"强校验，可排除 Opus 负载里 "OggS\x00" 的假阳性）。
// 返回 granule（48 kHz 域采样数）；找不到合法页返回 0（duration 0 语义）。
static uint64_t ogg_last_granule(void *fh, uint32_t fsize)
{
    if (fsize < 27u + 1u) {
        return 0;
    }
    const uint32_t window = fsize < BUZZ_OGG_SCAN_MAX ? fsize : BUZZ_OGG_SCAN_MAX;
    uint32_t covered = 0;
    while (covered < window) {
        const uint32_t rlen =
            window - covered > sizeof(s_chunk) ? (uint32_t)sizeof(s_chunk) : window - covered;
        const uint32_t off = fsize - covered - rlen;
        const long n = s_io.read_at(s_io.ctx, fh, off, s_chunk, rlen);
        if (n < (long)rlen) {
            return 0; // 尾扫读失败：按无合法页处理（duration 0，crc 另行计算不受影响）
        }
        // 块内从后向前找候选页首
        for (uint32_t i = rlen - 4u;; i--) {
            if (s_chunk[i] == 'O' && s_chunk[i + 1] == 'g' && s_chunk[i + 2] == 'g' &&
                s_chunk[i + 3] == 'S' && s_chunk[i + 4] == 0u) {
                const uint32_t page_off = off + i;
                // 读页头 + 段表（27 + nsegs 字节）；短读（候选贴近 EOF 等）
                // 一律视为假阳性，继续向前找
                const long hn = s_io.read_at(s_io.ctx, fh, page_off, s_page_hdr,
                                             (uint32_t)sizeof(s_page_hdr));
                if (hn >= 27) {
                    const uint32_t nsegs = s_page_hdr[26];
                    if (nsegs <= (uint32_t)hn - 27u) {
                        uint32_t body = 0;
                        for (uint32_t k = 0; k < nsegs; k++) {
                            body += s_page_hdr[27u + k];
                        }
                        const uint64_t page_total = 27u + nsegs + body;
                        if ((uint64_t)page_off + page_total == (uint64_t)fsize) {
                            return rd64le(s_page_hdr + 6); // 强校验通过：确为最后一页
                        }
                    }
                }
                // 假阳性（负载内 "OggS\x00"）：继续向前找
            }
            if (i == 0u) {
                break;
            }
        }
        covered += rlen;
    }
    return 0;
}

static uint32_t ogg_duration_ms(void *fh, uint32_t fsize)
{
    const uint64_t granule = ogg_last_granule(fh, fsize);
    return (uint32_t)((granule * 1000u) / BUZZ_OGG_GRANULE_HZ);
}

// ---------- 流式全文件 CRC ----------

static int file_crc16(void *fh, uint32_t fsize, uint16_t *out)
{
    uint16_t crc = BUZZ_CRC16_INIT;
    uint32_t off = 0;
    while (off < fsize) {
        const uint32_t want =
            fsize - off > sizeof(s_io_buf) ? (uint32_t)sizeof(s_io_buf) : fsize - off;
        const long n = s_io.read_at(s_io.ctx, fh, off, s_io_buf, want);
        if (n < 0) {
            return BUZZ_INDEX_ERR_IO;
        }
        if (n == 0) {
            break; // 短文件（声明 size 与实际不符）：按已读部分结算
        }
        crc = buzz_crc16_update(crc, s_io_buf, (size_t)n);
        off += (uint32_t)n;
    }
    *out = crc;
    return BUZZ_OK;
}

// ---------- 打开并解析（list 切片与 info 共用） ----------

static void *open_by_id(const char *id)
{
    return s_io.open(s_io.ctx, id);
}

// 填齐 entry 的 size/durationMs（按扩展名分派解析）；打开失败返回
// BUZZ_INDEX_ERR_NOT_FOUND，读失败返回 BUZZ_INDEX_ERR_IO。
static int fill_entry(buzz_file_entry_t *entry)
{
    void *fh = open_by_id(entry->id);
    if (fh == NULL) {
        return BUZZ_INDEX_ERR_NOT_FOUND;
    }
    const long fsize = s_io.size(s_io.ctx, fh);
    if (fsize < 0) {
        s_io.close(s_io.ctx, fh);
        return BUZZ_INDEX_ERR_IO;
    }
    entry->size = (uint32_t)fsize;
    if (is_ogg_name(entry->id)) {
        entry->durationMs = ogg_duration_ms(fh, (uint32_t)fsize);
    } else {
        uint32_t rate;
        uint32_t ch;
        wav_meta(fh, (uint32_t)fsize, &rate, &ch, &entry->durationMs);
    }
    s_io.close(s_io.ctx, fh);
    return BUZZ_OK;
}

// ---------- qsort 比较器：id 字典序降序（新→旧，D1） ----------

static int cmp_id_desc(const void *a, const void *b)
{
    const buzz_file_entry_t *ea = a;
    const buzz_file_entry_t *eb = b;
    return -strcmp(ea->id, eb->id);
}

// ---------- 索引重建 ----------

typedef struct {
    uint32_t count; // 已收集条数（达容量后忽略多余条目，total 另计）
} collect_ctx_t;

static void collect_cb(void *cb_ctx, const char *name)
{
    collect_ctx_t *c = cb_ctx;
    if (!buzz_file_index_id_match(name)) {
        return;
    }
    if (c->count < BUZZ_INDEX_MAX_FILES) {
        memset(&s_ids[c->count], 0, sizeof(s_ids[c->count]));
        strncpy(s_ids[c->count].id, name, BUZZ_FILE_ID_MAX_LEN);
        s_ids[c->count].id[BUZZ_FILE_ID_MAX_LEN] = '\0';
        c->count++;
    }
}

typedef struct {
    uint32_t total; // 匹配条目真实总数（不受容量截断影响）
} count_ctx_t;

static void count_cb(void *cb_ctx, const char *name)
{
    count_ctx_t *c = cb_ctx;
    if (buzz_file_index_id_match(name)) {
        c->total++;
    }
}

// ---------- 公开 API ----------

int buzz_file_index_init(const buzz_file_io_t *io)
{
    if (io == NULL || io->open == NULL || io->read_at == NULL || io->size == NULL ||
        io->close == NULL || io->listdir == NULL) {
        return BUZZ_ERR_NULL;
    }
    s_io = *io;
    s_ready = true;
    return BUZZ_OK;
}

int buzz_file_index_list(uint32_t offset, uint32_t limit, buzz_file_entry_t *entries,
                         uint32_t entries_cap, uint32_t *out_total, uint32_t *out_count)
{
    if (!s_ready || entries == NULL || out_total == NULL || out_count == NULL) {
        return BUZZ_ERR_NULL;
    }
    if (limit == 0u) {
        limit = BUZZ_INDEX_PAGE_MAX; // 缺省 16（§5.2）
    }
    if (limit > BUZZ_INDEX_PAGE_MAX) {
        limit = BUZZ_INDEX_PAGE_MAX; // 上限 16
    }
    if (entries_cap < limit) {
        return BUZZ_ERR_ARG;
    }

    // 第一遍：计数（total 为真实总数，可大于索引容量）
    count_ctx_t counter = {0};
    const long n_total = s_io.listdir(s_io.ctx, count_cb, &counter);
    if (n_total < 0) {
        return BUZZ_INDEX_ERR_IO;
    }

    // 第二遍：收集 + 降序排序
    collect_ctx_t collector = {0};
    const long n_list = s_io.listdir(s_io.ctx, collect_cb, &collector);
    if (n_list < 0) {
        return BUZZ_INDEX_ERR_IO;
    }
    const uint32_t stored = collector.count;
    qsort(s_ids, stored, sizeof(s_ids[0]), cmp_id_desc);

    // 切片 [offset, offset+limit) ∩ [0, stored)
    uint32_t count = 0;
    if (offset < stored) {
        count = stored - offset;
        if (count > limit) {
            count = limit;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        entries[i] = s_ids[offset + i]; // 先拷贝 id，再填元数据（失败则降级 0 值条目）
        (void)fill_entry(&entries[i]);
    }
    *out_total = counter.total;
    *out_count = count;
    return BUZZ_OK;
}

int buzz_file_index_info(const char *id, buzz_file_info_t *out)
{
    if (!s_ready || id == NULL || out == NULL) {
        return BUZZ_ERR_NULL;
    }
    if (strchr(id, '/') != NULL || strchr(id, '\\') != NULL) {
        return BUZZ_ERR_ARG; // 路径穿越防线：id 只应是裸文件名
    }
    memset(out, 0, sizeof(*out));

    void *fh = open_by_id(id);
    if (fh == NULL) {
        return BUZZ_INDEX_ERR_NOT_FOUND;
    }
    int ret = BUZZ_OK;
    const long fsize = s_io.size(s_io.ctx, fh);
    if (fsize < 0) {
        ret = BUZZ_INDEX_ERR_IO;
        goto done;
    }
    out->size = (uint32_t)fsize;
    ret = file_crc16(fh, (uint32_t)fsize, &out->crc16);
    if (ret != BUZZ_OK) {
        goto done;
    }
    if (is_ogg_name(id)) {
        strcpy(out->format, "ogg-opus"); // Opus in Ogg（RFC 7845）
        out->sampleRate = 16000u;        // M3 封装固定参数（OpusHead 不解析）
        out->channels = 1u;
        out->durationMs = ogg_duration_ms(fh, (uint32_t)fsize);
    } else {
        strcpy(out->format, "wav-pcm"); // 16 kHz/16 bit 单声道 PCM in RIFF
        wav_meta(fh, (uint32_t)fsize, &out->sampleRate, &out->channels, &out->durationMs);
    }
done:
    s_io.close(s_io.ctx, fh);
    return ret;
}
