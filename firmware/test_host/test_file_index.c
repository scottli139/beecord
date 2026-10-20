// file_index 单测（M4.3）：内存假文件系统注入（buzz_file_io_t 五件套），覆盖
//   - 枚举过滤（REC_ 前缀 + .WAV/.OGG 扩展名大小写不敏感）与 id 降序排序（新→旧）
//   - 分页：limit 缺省 16 / 上限 16 截断 / offset 翻页 / offset == total 空页
//   - WAV durationMs（RIFF fmt/data 解析、byteRate 换算、非 16000Hz 容错）
//   - OGG durationMs（多页取末页 granule/48k、>64KB 大文件尾扫、负载内
//     "OggS\x00" 假阳性强校验排除）
//   - 0 字节文件（size=0 / crc16=0xFFFF / duration=0 不崩溃）、缺文件、路径穿越
//   - crc16 已知向量（§3.2 "123456789"=0x29B1 / "Beecord"=0x5D1E）与流式等价性
//   - 损坏头容错（duration 0、crc 不受影响）、索引容量溢出（total 真实计数）
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crc16.h"
#include "file_index.h"
#include "test.h"
#include "wav_writer.h"

// ---------- 内存假文件系统 ----------

#define FAKE_FS_MAX 160

typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t size;
} fake_file_t;

static fake_file_t g_fs[FAKE_FS_MAX];
static int g_fs_count;
static long g_read_calls; // read_at 调用计数（CRC 缓存行为断言用）
static long g_read_bytes;

static void fs_reset(void)
{
    g_fs_count = 0;
    g_read_calls = 0;
    g_read_bytes = 0;
}

// 同名覆盖（构造"同名不同 size"场景的测试工具）
static void fs_add(const char *name, const uint8_t *data, uint32_t size)
{
    for (int i = 0; i < g_fs_count; i++) {
        if (strcmp(g_fs[i].name, name) == 0) {
            g_fs[i].data = data;
            g_fs[i].size = size;
            return;
        }
    }
    if (g_fs_count >= FAKE_FS_MAX) {
        fprintf(stderr, "fake fs full\n");
        return;
    }
    g_fs[g_fs_count].name = name;
    g_fs[g_fs_count].data = data;
    g_fs[g_fs_count].size = size;
    g_fs_count++;
}

static void *fake_open(void *ctx, const char *path)
{
    (void)ctx;
    for (int i = 0; i < g_fs_count; i++) {
        if (strcmp(g_fs[i].name, path) == 0) {
            return &g_fs[i];
        }
    }
    return NULL;
}

static long fake_read_at(void *ctx, void *fh, uint32_t off, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    fake_file_t *f = fh;
    g_read_calls++;
    if (off >= f->size) {
        return 0;
    }
    const uint32_t avail = f->size - off;
    if (len > avail) {
        len = avail;
    }
    g_read_bytes += (long)len;
    memcpy(buf, f->data + off, len);
    return (long)len;
}

static long fake_size(void *ctx, void *fh)
{
    (void)ctx;
    return (long)((fake_file_t *)fh)->size;
}

static void fake_close(void *ctx, void *fh)
{
    (void)ctx;
    (void)fh;
}

static long fake_listdir(void *ctx, buzz_file_dirent_cb_t cb, void *cb_ctx)
{
    (void)ctx;
    long n = 0;
    for (int i = 0; i < g_fs_count; i++) {
        cb(cb_ctx, g_fs[i].name);
        n++;
    }
    return n;
}

static const buzz_file_io_t g_fake_io = {
    .ctx = NULL,
    .open = fake_open,
    .read_at = fake_read_at,
    .size = fake_size,
    .close = fake_close,
    .listdir = fake_listdir,
};

static void fixture(void)
{
    fs_reset();
    CHECK_EQ_INT(buzz_file_index_init(&g_fake_io), BUZZ_OK);
}

// ---------- 合成文件构造器 ----------

// WAV：44B 标准头（buzz_wav_build_header）+ data_size 字节负载（内容固定 0x55）
static uint8_t g_wav[44 + 32768];
static uint32_t g_wav_len;

static void make_wav(uint32_t data_size)
{
    CHECK_EQ_INT(buzz_wav_build_header(g_wav, sizeof(g_wav), data_size), 44);
    memset(g_wav + 44, 0x55, data_size);
    g_wav_len = 44u + data_size;
}

// Ogg 页（单段；page CRC 置 0——被测代码不校验页 CRC）
static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr64le(uint8_t *p, uint64_t v)
{
    wr32le(p, (uint32_t)v);
    wr32le(p + 4, (uint32_t)(v >> 32));
}

// 在 out 写入一页，返回页总长（28 + plen）
static size_t ogg_page(uint8_t *out, uint64_t granule, uint8_t hdr_type,
                       const uint8_t *payload, uint8_t plen)
{
    memcpy(out, "OggS", 4);
    out[4] = 0; // stream structure version
    out[5] = hdr_type;
    wr64le(out + 6, granule);
    wr32le(out + 14, 0x12345678u); // serial
    wr32le(out + 18, 0u);          // seq（被测代码不校验）
    wr32le(out + 22, 0u);          // page CRC 未校验
    out[26] = 1;                   // 1 段
    out[27] = plen;
    memcpy(out + 28, payload, plen);
    return 28u + plen;
}

static uint8_t g_ogg[80 * 1024];
static uint32_t g_ogg_len;

static void reset_ogg(void)
{
    g_ogg_len = 0;
}

static void append_ogg_page(uint64_t granule, uint8_t hdr_type, const uint8_t *payload,
                            uint8_t plen)
{
    g_ogg_len += (uint32_t)ogg_page(g_ogg + g_ogg_len, granule, hdr_type, payload, plen);
}

static buzz_file_entry_t g_entries[BUZZ_INDEX_PAGE_MAX];

// ---------- 用例 ----------

static void test_filters_prefix_and_ext(void)
{
    fixture();
    fs_add("REC_0001.WAV", NULL, 0);
    fs_add("REC_0002.OGG", NULL, 0);
    fs_add("README.TXT", NULL, 0);   // 非 REC_ 前缀
    fs_add("rec_0003.wav", NULL, 0); // 前缀大小写不符
    fs_add("REC_0004.mp3", NULL, 0); // 扩展名不符
    fs_add("REC_0005.oGg", NULL, 0); // 扩展名大小写混合：应收录
    uint32_t total = 0;
    uint32_t count = 0;
    CHECK_EQ_INT(buzz_file_index_list(0, 16, g_entries, 16, &total, &count), BUZZ_OK);
    CHECK_EQ_INT(total, 3);
    CHECK_EQ_INT(count, 3);
    CHECK(strcmp(g_entries[0].id, "REC_0005.oGg") == 0);
    CHECK(strcmp(g_entries[1].id, "REC_0002.OGG") == 0);
    CHECK(strcmp(g_entries[2].id, "REC_0001.WAV") == 0);
}

static void test_sort_desc_mixed_ext(void)
{
    fixture();
    fs_add("REC_0003.OGG", NULL, 0);
    fs_add("REC_0010.WAV", NULL, 0);
    fs_add("REC_0002.WAV", NULL, 0);
    fs_add("REC_0010.OGG", NULL, 0);
    uint32_t total = 0;
    uint32_t count = 0;
    CHECK_EQ_INT(buzz_file_index_list(0, 16, g_entries, 16, &total, &count), BUZZ_OK);
    CHECK_EQ_INT(count, 4);
    CHECK(strcmp(g_entries[0].id, "REC_0010.WAV") == 0); // 字典序降序：同号 .WAV > .OGG
    CHECK(strcmp(g_entries[1].id, "REC_0010.OGG") == 0);
    CHECK(strcmp(g_entries[2].id, "REC_0003.OGG") == 0);
    CHECK(strcmp(g_entries[3].id, "REC_0002.WAV") == 0);
}

static void test_default_limit_is_16(void)
{
    static char names[20][13];
    fixture();
    for (int i = 0; i < 20; i++) {
        snprintf(names[i], sizeof(names[i]), "REC_%04d.OGG", i + 1);
        fs_add(names[i], NULL, 0);
    }
    uint32_t total = 0;
    uint32_t count = 0;
    CHECK_EQ_INT(buzz_file_index_list(0, 0, g_entries, 16, &total, &count), BUZZ_OK);
    CHECK_EQ_INT(total, 20);
    CHECK_EQ_INT(count, 16); // limit=0 → 缺省 16
    CHECK(strcmp(g_entries[0].id, "REC_0020.OGG") == 0);
}

static void test_limit_capped_at_16(void)
{
    static char names[20][13];
    fixture();
    for (int i = 0; i < 20; i++) {
        snprintf(names[i], sizeof(names[i]), "REC_%04d.WAV", i + 1);
        fs_add(names[i], NULL, 0);
    }
    uint32_t total = 0;
    uint32_t count = 0;
    CHECK_EQ_INT(buzz_file_index_list(0, 99, g_entries, 16, &total, &count), BUZZ_OK);
    CHECK_EQ_INT(count, 16); // limit>16 截为 16（§5.2）
}

static void test_paging_offset(void)
{
    static char names[20][13];
    fixture();
    for (int i = 0; i < 20; i++) {
        snprintf(names[i], sizeof(names[i]), "REC_%04d.OGG", i + 1);
        fs_add(names[i], NULL, 0);
    }
    uint32_t total = 0;
    uint32_t count = 0;
    CHECK_EQ_INT(buzz_file_index_list(16, 16, g_entries, 16, &total, &count), BUZZ_OK);
    CHECK_EQ_INT(total, 20);
    CHECK_EQ_INT(count, 4);
    CHECK(strcmp(g_entries[0].id, "REC_0004.OGG") == 0);
    CHECK(strcmp(g_entries[3].id, "REC_0001.OGG") == 0);
}

static void test_offset_eq_total_empty_page(void)
{
    fixture();
    fs_add("REC_0001.OGG", NULL, 0);
    uint32_t total = 0;
    uint32_t count = 99;
    CHECK_EQ_INT(buzz_file_index_list(1, 16, g_entries, 16, &total, &count), BUZZ_OK);
    CHECK_EQ_INT(total, 1);
    CHECK_EQ_INT(count, 0); // offset == total：空页（buzz_cmds 据此正常终止）
}

static void test_wav_duration_1s(void)
{
    fixture();
    make_wav(32000u); // 16kHz/16bit/单声道 = 32000 B/s → 1000 ms
    fs_add("REC_0001.WAV", g_wav, g_wav_len);
    buzz_file_info_t info;
    CHECK_EQ_INT(buzz_file_index_info("REC_0001.WAV", &info), BUZZ_OK);
    CHECK_EQ_INT(info.size, 44u + 32000u);
    CHECK_EQ_INT(info.durationMs, 1000u);
    CHECK_EQ_INT(info.sampleRate, 16000u);
    CHECK_EQ_INT(info.channels, 1u);
    CHECK(strcmp(info.format, "wav-pcm") == 0);
    CHECK_EQ_INT((int)info.crc16, (int)buzz_crc16(g_wav, g_wav_len)); // crc 覆盖整文件
}

static void test_wav_duration_respects_fmt(void)
{
    fixture();
    make_wav(16000u);
    // 改 fmt：8 kHz / byteRate 16000（保持 dataSize 16000 → 仍 1000 ms，
    // 证明 duration 走 byteRate 而非硬编码 32000）
    wr32le(g_wav + 24, 8000u);
    wr32le(g_wav + 28, 16000u);
    fs_add("REC_0001.WAV", g_wav, g_wav_len);
    buzz_file_info_t info;
    CHECK_EQ_INT(buzz_file_index_info("REC_0001.WAV", &info), BUZZ_OK);
    CHECK_EQ_INT(info.sampleRate, 8000u);
    CHECK_EQ_INT(info.durationMs, 1000u);
}

static void test_wav_crc_known_vector(void)
{
    fixture();
    // data 恰为 "123456789"：整文件 crc 应与手工"44B 头 + 该 9 字节"一致，
    // 且头外数据段自身的 crc 为 §3.2 向量 0x29B1
    make_wav(9u);
    memcpy(g_wav + 44, "123456789", 9u);
    fs_add("REC_0001.WAV", g_wav, g_wav_len);
    buzz_file_info_t info;
    CHECK_EQ_INT(buzz_file_index_info("REC_0001.WAV", &info), BUZZ_OK);
    CHECK_EQ_INT((int)buzz_crc16((const uint8_t *)"123456789", 9), 0x29B1);
    CHECK_EQ_INT((int)info.crc16, (int)buzz_crc16(g_wav, g_wav_len));
}

static void test_ogg_duration_multipage(void)
{
    fixture();
    reset_ogg();
    static const uint8_t payload[100] = {0};
    append_ogg_page(48000u, 0x02, payload, sizeof(payload));     // 第 1 页（BOS）
    append_ogg_page(96000u, 0x00, payload, sizeof(payload));     // 中间页
    append_ogg_page(48000ull * 12345u / 1000u, 0x04, payload, 10); // EOS 末页：granule
                                                                   // = 12345 ms × 48
    fs_add("REC_0001.OGG", g_ogg, g_ogg_len);
    buzz_file_info_t info;
    CHECK_EQ_INT(buzz_file_index_info("REC_0001.OGG", &info), BUZZ_OK);
    CHECK_EQ_INT(info.durationMs, 12345u);
    CHECK_EQ_INT(info.sampleRate, 16000u);
    CHECK_EQ_INT(info.channels, 1u);
    CHECK(strcmp(info.format, "ogg-opus") == 0);
    CHECK_EQ_INT((int)info.crc16, (int)buzz_crc16(g_ogg, g_ogg_len));
}

static void test_ogg_large_file_tail_scan(void)
{
    fixture();
    static uint8_t big[70000];
    static const uint8_t payload[100] = {0};
    static const uint8_t payload_last[200u - 28u] = {0};
    size_t off = ogg_page(big, 0u, 0x02, payload, sizeof(payload)); // 文件头页
    memset(big + off, 0xAA, sizeof(big) - off - 200u);              // 填充
    // 末页贴文件尾：granule = 48000*100 s
    ogg_page(big + sizeof(big) - 200u, 48000ull * 100u, 0x04, payload_last,
             sizeof(payload_last));
    fs_add("REC_0001.OGG", big, sizeof(big));
    buzz_file_info_t info;
    CHECK_EQ_INT(buzz_file_index_info("REC_0001.OGG", &info), BUZZ_OK);
    CHECK_EQ_INT(info.durationMs, 100000u); // 尾扫窗口 64KB 内定位末页
    CHECK_EQ_INT(info.size, sizeof(big));
}

static void test_ogg_false_oggs_in_payload(void)
{
    fixture();
    // 末页负载内埋 "OggS\x00" + 更多字节：反向扫描先命中假候选，
    // 其"段表/总长"无法通过 exact-EOF 强校验，必须回退到真页首
    uint8_t payload[60];
    memset(payload, 0x5A, sizeof(payload));
    memcpy(payload + 10, "OggS\x00\xff", 6);
    reset_ogg();
    append_ogg_page(48000ull * 40000u / 1000u, 0x04, payload, sizeof(payload));
    fs_add("REC_0001.OGG", g_ogg, g_ogg_len);
    buzz_file_info_t info;
    CHECK_EQ_INT(buzz_file_index_info("REC_0001.OGG", &info), BUZZ_OK);
    CHECK_EQ_INT(info.durationMs, 40000u);
}

static void test_zero_byte_files(void)
{
    fixture();
    fs_add("REC_0001.OGG", NULL, 0);
    fs_add("REC_0002.WAV", NULL, 0);
    buzz_file_info_t ogg;
    buzz_file_info_t wav;
    CHECK_EQ_INT(buzz_file_index_info("REC_0001.OGG", &ogg), BUZZ_OK);
    CHECK_EQ_INT(ogg.size, 0u);
    CHECK_EQ_INT((int)ogg.crc16, (int)BUZZ_CRC16_INIT); // 空输入 CRC = 0xFFFF
    CHECK_EQ_INT(ogg.durationMs, 0u);
    CHECK_EQ_INT(ogg.sampleRate, 16000u); // OGG 固定参数照常报告
    CHECK_EQ_INT(buzz_file_index_info("REC_0002.WAV", &wav), BUZZ_OK);
    CHECK_EQ_INT(wav.size, 0u);
    CHECK_EQ_INT((int)wav.crc16, (int)BUZZ_CRC16_INIT);
    CHECK_EQ_INT(wav.durationMs, 0u);
    CHECK_EQ_INT(wav.sampleRate, 0u); // WAV 头缺失 → 0，不崩溃
    uint32_t total = 0;
    uint32_t count = 0;
    CHECK_EQ_INT(buzz_file_index_list(0, 16, g_entries, 16, &total, &count), BUZZ_OK);
    CHECK_EQ_INT(count, 2);
    CHECK_EQ_INT(g_entries[0].size, 0u);
    CHECK_EQ_INT(g_entries[1].size, 0u);
}

static void test_info_not_found(void)
{
    fixture();
    fs_add("REC_0001.OGG", NULL, 0);
    buzz_file_info_t info;
    CHECK_EQ_INT(buzz_file_index_info("REC_9999.OGG", &info), BUZZ_INDEX_ERR_NOT_FOUND);
    CHECK_EQ_INT(buzz_file_index_info("README.TXT", &info), BUZZ_INDEX_ERR_NOT_FOUND);
}

static void test_info_path_rejected(void)
{
    fixture();
    fs_add("REC_0001.OGG", NULL, 0);
    buzz_file_info_t info;
    CHECK_EQ_INT(buzz_file_index_info("x/y.OGG", &info), BUZZ_ERR_ARG);
    CHECK_EQ_INT(buzz_file_index_info("x\\y.OGG", &info), BUZZ_ERR_ARG);
}

static void test_crc16_streaming_equivalence(void)
{
    fixture();
    static uint8_t big[5000];
    for (uint32_t i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 31u + 7u);
    }
    fs_add("REC_0001.OGG", big, sizeof(big));
    buzz_file_info_t info;
    CHECK_EQ_INT(buzz_file_index_info("REC_0001.OGG", &info), BUZZ_OK);
    CHECK_EQ_INT((int)info.crc16, (int)buzz_crc16(big, sizeof(big)));
    // 手工奇数分块流式累计应与一次性全等（512B 内部缓冲跨块语义）
    uint16_t crc = BUZZ_CRC16_INIT;
    uint32_t off = 0;
    const uint8_t steps[4] = {1, 3, 7, 113};
    uint32_t s = 0;
    while (off < sizeof(big)) {
        const uint32_t n = sizeof(big) - off > steps[s] ? steps[s] : (uint32_t)sizeof(big) - off;
        crc = buzz_crc16_update(crc, big + off, n);
        off += n;
        s = (s + 1u) % 4u;
    }
    CHECK_EQ_INT((int)info.crc16, (int)crc);
}

static void test_crc16_beecord_vector(void)
{
    CHECK_EQ_INT((int)buzz_crc16((const uint8_t *)"Beecord", 7), 0x5D1E); // §3.2 向量
}

static void test_corrupt_headers_tolerated(void)
{
    fixture();
    static uint8_t junk_wav[200];
    static uint8_t junk_ogg[200];
    memset(junk_wav, 0x11, sizeof(junk_wav)); // 非 RIFF
    memset(junk_ogg, 0x22, sizeof(junk_ogg)); // 无 OggS
    fs_add("REC_0001.WAV", junk_wav, sizeof(junk_wav));
    fs_add("REC_0002.OGG", junk_ogg, sizeof(junk_ogg));
    buzz_file_info_t wav;
    buzz_file_info_t ogg;
    CHECK_EQ_INT(buzz_file_index_info("REC_0001.WAV", &wav), BUZZ_OK);
    CHECK_EQ_INT(wav.durationMs, 0u); // 头损坏不报错
    CHECK_EQ_INT(wav.sampleRate, 0u);
    CHECK_EQ_INT((int)wav.crc16, (int)buzz_crc16(junk_wav, sizeof(junk_wav)));
    CHECK_EQ_INT(buzz_file_index_info("REC_0002.OGG", &ogg), BUZZ_OK);
    CHECK_EQ_INT(ogg.durationMs, 0u);
    CHECK_EQ_INT((int)ogg.crc16, (int)buzz_crc16(junk_ogg, sizeof(junk_ogg)));
}

static void test_index_overflow_reports_true_total(void)
{
    static char names[130][13];
    fixture();
    for (int i = 0; i < 130; i++) { // 超出索引容量 128
        snprintf(names[i], sizeof(names[i]), "REC_%04d.OGG", i + 1);
        fs_add(names[i], NULL, 0);
    }
    uint32_t total = 0;
    uint32_t count = 0;
    CHECK_EQ_INT(buzz_file_index_list(0, 16, g_entries, 16, &total, &count), BUZZ_OK);
    CHECK_EQ_INT(total, 130u); // total 为真实计数
    CHECK_EQ_INT(count, 16u);
    CHECK_EQ_INT(buzz_file_index_list(124, 16, g_entries, 16, &total, &count), BUZZ_OK);
    CHECK_EQ_INT(count, 4u); // 可用条目截在 128：切片 [124,128)
}

// ---------- CRC 缓存（FILE_INFO / DL_BEGIN 共享） ----------

// 冷查询（未命中）应读完整个文件；热查询（命中）只读解析所需窗口
static void test_crc_cache_hit_skips_full_read(void)
{
    fixture();
    make_wav(20000u);
    fs_add("REC_0001.WAV", g_wav, g_wav_len);
    buzz_file_info_t info;
    CHECK_EQ_INT(buzz_file_index_info("REC_0001.WAV", &info), BUZZ_OK);
    const long cold_bytes = g_read_bytes;
    CHECK(cold_bytes >= (long)g_wav_len); // 冷 = 头窗口 + 全文件 CRC
    const uint16_t crc_cold = info.crc16;
    const long calls_cold = g_read_calls;

    CHECK_EQ_INT(buzz_file_index_info("REC_0001.WAV", &info), BUZZ_OK);
    CHECK_EQ_INT((int)info.crc16, (int)crc_cold); // 命中值一致
    CHECK(g_read_bytes - cold_bytes < (long)g_wav_len); // 未再读全文件
    CHECK(g_read_calls - calls_cold < calls_cold);      // 且读次数更少
}

static void test_crc_cache_size_change_invalidates(void)
{
    fixture();
    make_wav(100u);
    fs_add("REC_0001.WAV", g_wav, g_wav_len);
    buzz_file_info_t info;
    CHECK_EQ_INT(buzz_file_index_info("REC_0001.WAV", &info), BUZZ_OK); // 入缓存
    const uint16_t crc_old = info.crc16;

    static uint8_t wav2[44 + 5000];
    CHECK_EQ_INT(buzz_wav_build_header(wav2, sizeof(wav2), 5000u), 44);
    memset(wav2 + 44, 0x77, 5000u);
    fs_add("REC_0001.WAV", wav2, sizeof(wav2)); // 同名不同 size（写一次不改性被破坏场景）
    CHECK_EQ_INT(buzz_file_index_info("REC_0001.WAV", &info), BUZZ_OK);
    CHECK_EQ_INT(info.size, sizeof(wav2));
    CHECK((int)info.crc16 != (int)crc_old); // 缓存未误命中
    CHECK_EQ_INT((int)info.crc16, (int)buzz_crc16(wav2, sizeof(wav2)));
    // 新 key 已入库：再次查询只剩 ~1KB 头窗口读（无整文件 CRC 读）
    const long bytes = g_read_bytes;
    CHECK_EQ_INT(buzz_file_index_info("REC_0001.WAV", &info), BUZZ_OK);
    CHECK_EQ_INT((int)info.crc16, (int)buzz_crc16(wav2, sizeof(wav2)));
    CHECK(g_read_bytes - bytes <= 1024L);
}

static void test_crc_cache_shared_entry_point(void)
{
    fixture();
    make_wav(50u);
    fs_add("REC_0001.WAV", g_wav, g_wav_len);
    uint16_t crc = 0;
    CHECK_EQ_INT(buzz_file_index_crc16("REC_0001.WAV", g_wav_len, &crc), BUZZ_OK);
    CHECK_EQ_INT((int)crc, (int)buzz_crc16(g_wav, g_wav_len));
    CHECK(g_read_bytes >= (long)g_wav_len); // 冷：全读
    const long bytes = g_read_bytes;
    uint16_t crc2 = 0;
    CHECK_EQ_INT(buzz_file_index_crc16("REC_0001.WAV", g_wav_len, &crc2), BUZZ_OK);
    CHECK_EQ_INT((int)crc2, (int)crc);
    CHECK_EQ_INT(g_read_bytes, bytes); // 热：零读
    // size 参数与实不符 → NOT_FOUND（key 不成立）
    CHECK_EQ_INT(buzz_file_index_crc16("REC_0001.WAV", g_wav_len + 1u, &crc2),
                 BUZZ_INDEX_ERR_NOT_FOUND);
    CHECK_EQ_INT(buzz_file_index_crc16("REC_9999.OGG", 0u, &crc2),
                 BUZZ_INDEX_ERR_NOT_FOUND);
    // io 访问器：buzz_dl 经此取注入表直接读文件（非空且回调可用）
    const buzz_file_io_t *io = buzz_file_index_io();
    CHECK(io != NULL);
    if (io != NULL) {
        void *fh = io->open(io->ctx, "REC_0001.WAV");
        CHECK(fh != NULL);
        if (fh != NULL) {
            io->close(io->ctx, fh);
        }
    }
}

static void test_crc_cache_eviction_round_robin(void)
{
    static char names[33][13];
    fixture();
    make_wav(20u); // 共享内容即可：key 按名字区分
    for (int i = 0; i < 33; i++) {
        snprintf(names[i], sizeof(names[i]), "REC_%04d.WAV", i + 1);
        fs_add(names[i], g_wav, g_wav_len);
        uint16_t crc = 0;
        CHECK_EQ_INT(buzz_file_index_crc16(names[i], g_wav_len, &crc), BUZZ_OK);
    } // 容量 32：第 33 项覆盖最旧槽（name_0）
    const long bytes = g_read_bytes;
    uint16_t crc = 0;
    CHECK_EQ_INT(buzz_file_index_crc16(names[0], g_wav_len, &crc), BUZZ_OK); // miss
    CHECK(g_read_bytes > bytes);
    const long bytes2 = g_read_bytes;
    CHECK_EQ_INT(buzz_file_index_crc16(names[32], g_wav_len, &crc), BUZZ_OK); // hit
    CHECK_EQ_INT(g_read_bytes, bytes2);
}

int main(void)
{
    RUN_TEST(test_filters_prefix_and_ext);
    RUN_TEST(test_sort_desc_mixed_ext);
    RUN_TEST(test_default_limit_is_16);
    RUN_TEST(test_limit_capped_at_16);
    RUN_TEST(test_paging_offset);
    RUN_TEST(test_offset_eq_total_empty_page);
    RUN_TEST(test_wav_duration_1s);
    RUN_TEST(test_wav_duration_respects_fmt);
    RUN_TEST(test_wav_crc_known_vector);
    RUN_TEST(test_ogg_duration_multipage);
    RUN_TEST(test_ogg_large_file_tail_scan);
    RUN_TEST(test_ogg_false_oggs_in_payload);
    RUN_TEST(test_zero_byte_files);
    RUN_TEST(test_info_not_found);
    RUN_TEST(test_info_path_rejected);
    RUN_TEST(test_crc16_streaming_equivalence);
    RUN_TEST(test_crc16_beecord_vector);
    RUN_TEST(test_corrupt_headers_tolerated);
    RUN_TEST(test_index_overflow_reports_true_total);
    RUN_TEST(test_crc_cache_hit_skips_full_read);
    RUN_TEST(test_crc_cache_size_change_invalidates);
    RUN_TEST(test_crc_cache_shared_entry_point);
    RUN_TEST(test_crc_cache_eviction_round_robin);
    return test_report();
}
