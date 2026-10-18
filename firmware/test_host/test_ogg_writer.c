// Ogg 页封装写入器单测：页结构断言 + 每页 CRC32 独立验算（测试内位运算实现，
// 与 writer 的查表实现不同源）+ 端到端落盘与 tools/ogg_check.py 对拍。
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ogg_writer.h"
#include "test.h"

#define MAX_PAGES 128u
#define PAGE_BUF_CAP 4096u
#define TEST_SERIAL 0xBEEF1234u

static uint8_t s_page_buf[PAGE_BUF_CAP];
static uint8_t s_pages[MAX_PAGES][PAGE_BUF_CAP];
static uint32_t s_page_lens[MAX_PAGES];
static int s_page_count;

static void capture_page(const uint8_t *page, uint32_t len, void *ctx)
{
    (void)ctx;
    CHECK(s_page_count < (int)MAX_PAGES);
    if (s_page_count >= (int)MAX_PAGES) {
        return;
    }
    memcpy(s_pages[s_page_count], page, len);
    s_page_lens[s_page_count] = len;
    s_page_count++;
}

static void setup(void)
{
    s_page_count = 0;
}

// 测试内独立 CRC32（位运算直连型，与 writer 查表实现不同源，交叉验证）
static uint32_t test_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc;
}

// ---------- 页字段读取 ----------

static uint8_t page_header_type(int i) { return s_pages[i][5]; }

static uint64_t page_granule(int i)
{
    uint64_t v = 0;
    for (int k = 7; k >= 0; k--) {
        v = (v << 8) | s_pages[i][6 + k];
    }
    return v;
}

static uint32_t page_serial(int i)
{
    return (uint32_t)s_pages[i][14] | ((uint32_t)s_pages[i][15] << 8) |
           ((uint32_t)s_pages[i][16] << 16) | ((uint32_t)s_pages[i][17] << 24);
}

static uint32_t page_seq(int i)
{
    return (uint32_t)s_pages[i][18] | ((uint32_t)s_pages[i][19] << 8) |
           ((uint32_t)s_pages[i][20] << 16) | ((uint32_t)s_pages[i][21] << 24);
}

static uint32_t page_stored_crc(int i)
{
    return (uint32_t)s_pages[i][22] | ((uint32_t)s_pages[i][23] << 8) |
           ((uint32_t)s_pages[i][24] << 16) | ((uint32_t)s_pages[i][25] << 24);
}

static uint8_t page_nseg(int i) { return s_pages[i][26]; }

static const uint8_t *page_seg_table(int i) { return s_pages[i] + 27; }

static uint32_t page_payload_len(int i)
{
    uint32_t n = 0;
    for (uint8_t k = 0; k < page_nseg(i); k++) {
        n += page_seg_table(i)[k];
    }
    return n;
}

// 结构 + CRC 校验（对捕获的每一页调用；CRC 计算时 crc 字段须清零）
static void check_page_valid(int i)
{
    CHECK(memcmp(s_pages[i], "OggS", 4) == 0);
    CHECK_EQ_INT(s_pages[i][4], 0); // version
    CHECK_EQ_INT(page_serial(i), TEST_SERIAL);
    static uint8_t scratch[PAGE_BUF_CAP];
    memcpy(scratch, s_pages[i], s_page_lens[i]);
    memset(scratch + 22, 0, 4); // crc 字段清零后重算
    CHECK_EQ_INT((long)page_stored_crc(i), (long)test_crc32(scratch, s_page_lens[i]));
}

static void check_all_pages(void)
{
    for (int i = 0; i < s_page_count; i++) {
        check_page_valid(i);
    }
    // 页序号连续
    for (int i = 1; i < s_page_count; i++) {
        CHECK_EQ_INT((long)page_seq(i), (long)(page_seq(i - 1) + 1u));
    }
}

// ---------- 用例 ----------

static void test_opus_head_content(void)
{
    uint8_t out[32];
    int n = buzz_ogg_opus_head(out, sizeof(out));
    CHECK_EQ_INT(n, 19);
    static const uint8_t expect[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
                                       0x01, 0x01, 0x38, 0x01,             // ver, ch, preskip=312
                                       0x80, 0x3E, 0x00, 0x00,           // rate=16000
                                       0x00, 0x00,                        // gain
                                       0x00};                             // mapping
    CHECK(memcmp(out, expect, 19) == 0);
    CHECK_EQ_INT(buzz_ogg_opus_head(out, 18), BUZZ_ERR_SIZE);
}

static void test_opus_tags_content(void)
{
    uint8_t out[32];
    int n = buzz_ogg_opus_tags(out, sizeof(out), NULL); // 默认 vendor
    CHECK_EQ_INT(n, 23);                                // 8 + 4 + 7("beecord") + 4
    CHECK(memcmp(out, "OpusTags", 8) == 0);
    CHECK_EQ_INT(out[8], 7u); // vendor len LE
    CHECK(memcmp(out + 12, "beecord", 7) == 0);
    CHECK_EQ_INT(out[19], 0u); // 0 条注释（count LE 第 1 字节）
    CHECK_EQ_INT(buzz_ogg_opus_tags(out, 22, NULL), BUZZ_ERR_SIZE);
}

static void test_basic_stream_structure(void)
{
    setup();
    buzz_ogg_writer_t w;
    CHECK_EQ_INT(buzz_ogg_writer_init(&w, TEST_SERIAL, s_page_buf, PAGE_BUF_CAP,
                                      capture_page, NULL),
                 BUZZ_OK);
    uint8_t head[32], tags[32];
    int head_len = buzz_ogg_opus_head(head, sizeof(head));
    int tags_len = buzz_ogg_opus_tags(tags, sizeof(tags), NULL);
    uint8_t pkt[100] = {0};
    for (int i = 0; i < 100; i++) {
        pkt[i] = (uint8_t)i;
    }

    CHECK_EQ_INT(buzz_ogg_write_packet(&w, head, (uint32_t)head_len, 0, BUZZ_OGG_FLAG_BOS), BUZZ_OK);
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, tags, (uint32_t)tags_len, 0, 0), BUZZ_OK);
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 320, 0), BUZZ_OK);
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 640, 0), BUZZ_OK);
    CHECK_EQ_INT(s_page_count, 0); // 未满页不自动产出
    CHECK_EQ_INT(buzz_ogg_finalize(&w, 640), BUZZ_OK);

    CHECK_EQ_INT(s_page_count, 2);
    // 首页：BOS、无 EOS、四段（19+23+100+100）、granule = 末包 640
    CHECK((page_header_type(0) & BUZZ_OGG_HEADER_BOS) != 0u);
    CHECK((page_header_type(0) & BUZZ_OGG_HEADER_EOS) == 0u);
    CHECK_EQ_INT(page_seq(0), 0);
    CHECK_EQ_INT(page_nseg(0), 4);
    CHECK_EQ_INT((long)page_payload_len(0), 19 + 23 + 100 + 100);
    CHECK_EQ_INT((long)page_granule(0), 640);
    // 末页：空 EOS、granule 取末包值
    CHECK((page_header_type(1) & BUZZ_OGG_HEADER_EOS) != 0u);
    CHECK_EQ_INT(page_nseg(1), 0);
    CHECK_EQ_INT((long)page_granule(1), 640);
    CHECK_EQ_INT((long)page_seq(1), 1);
    check_all_pages();
}

static void test_exact_255_packet_gets_zero_terminator(void)
{
    setup();
    buzz_ogg_writer_t w;
    uint8_t small_buf[BUZZ_OGG_PAGE_BUF_MIN];
    CHECK_EQ_INT(buzz_ogg_writer_init(&w, TEST_SERIAL, small_buf, sizeof(small_buf),
                                      capture_page, NULL),
                 BUZZ_OK);
    uint8_t pkt[255];
    memset(pkt, 0xAB, sizeof(pkt));
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 320, 0), BUZZ_OK);
    CHECK_EQ_INT(buzz_ogg_finalize(&w, 320), BUZZ_OK);

    CHECK_EQ_INT(s_page_count, 2);
    CHECK_EQ_INT(page_nseg(0), 2);           // [255, 0]：整除补 0 段
    CHECK_EQ_INT(page_seg_table(0)[0], 255);
    CHECK_EQ_INT(page_seg_table(0)[1], 0);
    CHECK_EQ_INT((long)page_payload_len(0), 255);
    check_all_pages();
}

static void test_oversize_packet_spans_pages(void)
{
    setup();
    buzz_ogg_writer_t w;
    uint8_t small_buf[BUZZ_OGG_PAGE_BUF_MIN]; // payload 上限 255：520B 包强制跨 3 页
    CHECK_EQ_INT(buzz_ogg_writer_init(&w, TEST_SERIAL, small_buf, sizeof(small_buf),
                                      capture_page, NULL),
                 BUZZ_OK);
    uint8_t pkt[520]; // 255 + 255 + 10
    for (int i = 0; i < 520; i++) {
        pkt[i] = (uint8_t)(i * 31 + 7);
    }
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 640, 0), BUZZ_OK);
    CHECK_EQ_INT(buzz_ogg_finalize(&w, 640), BUZZ_OK);

    CHECK_EQ_INT(s_page_count, 4); // 3 数据页 + 1 空 EOS
    // 页 0/1：各一段 255，granule = -1（包未完），后续页置续传位
    for (int i = 0; i < 2; i++) {
        CHECK_EQ_INT(page_nseg(i), 1);
        CHECK_EQ_INT(page_seg_table(i)[0], 255);
        CHECK_EQ_INT((long)page_granule(i), -1L); // 0xFFFFFFFFFFFFFFFF
        CHECK((page_header_type(i + 1) & BUZZ_OGG_HEADER_CONTINUED) != 0u);
    }
    // 页 2：终结段 10，granule = 640；末页 EOS
    CHECK_EQ_INT(page_nseg(2), 1);
    CHECK_EQ_INT(page_seg_table(2)[0], 10);
    CHECK((page_header_type(2) & BUZZ_OGG_HEADER_CONTINUED) != 0u);
    CHECK_EQ_INT((long)page_granule(2), 640);
    CHECK((page_header_type(3) & BUZZ_OGG_HEADER_EOS) != 0u);
    // 跨页数据完整还原
    CHECK(memcmp(s_pages[0] + 27 + 1, pkt, 255) == 0);
    CHECK(memcmp(s_pages[1] + 27 + 1, pkt + 255, 255) == 0);
    CHECK(memcmp(s_pages[2] + 27 + 1, pkt + 510, 10) == 0);
    check_all_pages();
}

static void test_multi_packet_single_page(void)
{
    setup();
    buzz_ogg_writer_t w;
    CHECK_EQ_INT(buzz_ogg_writer_init(&w, TEST_SERIAL, s_page_buf, PAGE_BUF_CAP,
                                      capture_page, NULL),
                 BUZZ_OK);
    uint8_t pkt[20];
    for (int i = 1; i <= 10; i++) {
        memset(pkt, (uint8_t)i, sizeof(pkt));
        CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), (uint64_t)i * 320u, 0), BUZZ_OK);
    }
    CHECK_EQ_INT(buzz_ogg_finalize(&w, 3200), BUZZ_OK);
    CHECK_EQ_INT(s_page_count, 2);
    CHECK_EQ_INT(page_nseg(0), 10);
    CHECK_EQ_INT((long)page_granule(0), 3200); // granule = 页内末包
    check_all_pages();
}

static void test_empty_packet(void)
{
    setup();
    buzz_ogg_writer_t w;
    CHECK_EQ_INT(buzz_ogg_writer_init(&w, TEST_SERIAL, s_page_buf, PAGE_BUF_CAP,
                                      capture_page, NULL),
                 BUZZ_OK);
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, NULL, 0, 100, 0), BUZZ_OK); // 空包 = 单 0 段
    uint8_t pkt[50] = {0x5A};
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 200, 0), BUZZ_OK);
    CHECK_EQ_INT(buzz_ogg_finalize(&w, 200), BUZZ_OK);

    CHECK_EQ_INT(s_page_count, 2);
    CHECK_EQ_INT(page_nseg(0), 2); // [0, 50]
    CHECK_EQ_INT(page_seg_table(0)[0], 0);
    CHECK_EQ_INT(page_seg_table(0)[1], 50);
    CHECK_EQ_INT((long)page_granule(0), 200);
    check_all_pages();
}

static void test_flush_then_finalize(void)
{
    setup();
    buzz_ogg_writer_t w;
    CHECK_EQ_INT(buzz_ogg_writer_init(&w, TEST_SERIAL, s_page_buf, PAGE_BUF_CAP,
                                      capture_page, NULL),
                 BUZZ_OK);
    uint8_t pkt[40] = {0};
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 320, 0), BUZZ_OK);
    CHECK_EQ_INT(buzz_ogg_flush_page(&w), BUZZ_OK);
    CHECK_EQ_INT(s_page_count, 1);
    CHECK((page_header_type(0) & BUZZ_OGG_HEADER_EOS) == 0u); // flush 页无 EOS
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 640, 0), BUZZ_OK);
    CHECK_EQ_INT(buzz_ogg_finalize(&w, 640), BUZZ_OK);
    CHECK_EQ_INT(s_page_count, 3); // flush 页 + 数据页 + 空 EOS 页
    CHECK((page_header_type(2) & BUZZ_OGG_HEADER_EOS) != 0u);
    CHECK_EQ_INT((long)page_granule(2), 640);
    check_all_pages();
}

static void test_eos_via_packet_flag_finalize_idempotent(void)
{
    setup();
    buzz_ogg_writer_t w;
    CHECK_EQ_INT(buzz_ogg_writer_init(&w, TEST_SERIAL, s_page_buf, PAGE_BUF_CAP,
                                      capture_page, NULL),
                 BUZZ_OK);
    uint8_t pkt[40] = {0};
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 320, BUZZ_OGG_FLAG_BOS), BUZZ_OK);
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 640, BUZZ_OGG_FLAG_EOS), BUZZ_OK);
    CHECK_EQ_INT(s_page_count, 1);
    CHECK((page_header_type(0) & BUZZ_OGG_HEADER_BOS) != 0u);
    CHECK((page_header_type(0) & BUZZ_OGG_HEADER_EOS) != 0u);
    CHECK_EQ_INT(buzz_ogg_finalize(&w, 640), BUZZ_OK); // 幂等空操作，不重复产页
    CHECK_EQ_INT(s_page_count, 1);
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 960, 0), BUZZ_ERR_ARG); // 已结束
    check_all_pages();
}

static void test_granule_regression_rejected(void)
{
    setup();
    buzz_ogg_writer_t w;
    CHECK_EQ_INT(buzz_ogg_writer_init(&w, TEST_SERIAL, s_page_buf, PAGE_BUF_CAP,
                                      capture_page, NULL),
                 BUZZ_OK);
    uint8_t pkt[40] = {0};
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 640, BUZZ_OGG_FLAG_BOS), BUZZ_OK);
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 320, 0), BUZZ_ERR_ARG); // 回退
    CHECK_EQ_INT(buzz_ogg_finalize(&w, 100), BUZZ_ERR_ARG);                          // 回退
    CHECK_EQ_INT(buzz_ogg_finalize(&w, 640), BUZZ_OK);
    CHECK_EQ_INT(s_page_count, 2);
}

static void test_bos_second_packet_rejected(void)
{
    setup();
    buzz_ogg_writer_t w;
    CHECK_EQ_INT(buzz_ogg_writer_init(&w, TEST_SERIAL, s_page_buf, PAGE_BUF_CAP,
                                      capture_page, NULL),
                 BUZZ_OK);
    uint8_t pkt[40] = {0};
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 320, BUZZ_OGG_FLAG_BOS), BUZZ_OK);
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, sizeof(pkt), 640, BUZZ_OGG_FLAG_BOS),
                 BUZZ_ERR_ARG);
    CHECK_EQ_INT(s_page_count, 0);
}

static void test_init_guard(void)
{
    buzz_ogg_writer_t w;
    uint8_t small[BUZZ_OGG_PAGE_BUF_MIN];
    CHECK_EQ_INT(buzz_ogg_writer_init(&w, TEST_SERIAL, NULL, PAGE_BUF_CAP, capture_page, NULL),
                 BUZZ_ERR_ARG);
    CHECK_EQ_INT(buzz_ogg_writer_init(&w, TEST_SERIAL, small, BUZZ_OGG_PAGE_BUF_MIN - 1u,
                                      capture_page, NULL),
                 BUZZ_ERR_ARG);
}

// 端到端：OpusHead + OpusTags + 50 个伪随机包 → 完整 .ogg 落盘（供 ogg_check.py 对拍）
static void test_e2e_write_file(void)
{
    setup();
    buzz_ogg_writer_t w;
    CHECK_EQ_INT(buzz_ogg_writer_init(&w, TEST_SERIAL, s_page_buf, PAGE_BUF_CAP,
                                      capture_page, NULL),
                 BUZZ_OK);
    uint8_t head[32], tags[32];
    int head_len = buzz_ogg_opus_head(head, sizeof(head));
    int tags_len = buzz_ogg_opus_tags(tags, sizeof(tags), NULL);
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, head, (uint32_t)head_len, 0, BUZZ_OGG_FLAG_BOS), BUZZ_OK);
    CHECK_EQ_INT(buzz_ogg_write_packet(&w, tags, (uint32_t)tags_len, 0, 0), BUZZ_OK);

    uint32_t lcg = 0x12345678u;
    uint8_t pkt[2000];
    for (int i = 0; i < 50; i++) {
        uint32_t len = 200u + (uint32_t)((lcg = lcg * 1103515245u + 12345u) >> 16) % 1800u;
        for (uint32_t k = 0; k < len; k++) {
            lcg = lcg * 1103515245u + 12345u;
            pkt[k] = (uint8_t)(lcg >> 16);
        }
        CHECK_EQ_INT(buzz_ogg_write_packet(&w, pkt, len, (uint64_t)(i + 1) * 320u, 0), BUZZ_OK);
    }
    CHECK_EQ_INT(buzz_ogg_finalize(&w, 50u * 320u), BUZZ_OK);

    CHECK(s_page_count >= 3); // 多页 + EOS
    CHECK((page_header_type(0) & BUZZ_OGG_HEADER_BOS) != 0u);
    CHECK((page_header_type(s_page_count - 1) & BUZZ_OGG_HEADER_EOS) != 0u);
    // granule 单调（跳过 -1 的跨页页）
    uint64_t last = 0;
    for (int i = 0; i < s_page_count; i++) {
        uint64_t g = page_granule(i);
        CHECK(g == UINT64_MAX || g >= last);
        if (g != UINT64_MAX) {
            last = g;
        }
    }
    check_all_pages();

    // 落盘：完整 .ogg + 首页 CRC 翻转的坏页文件（ogg_check 对拍/反向验证用）
    FILE *good = fopen("build/test_e2e.ogg", "wb");
    CHECK(good != NULL);
    FILE *bad = fopen("build/test_bad_page.ogg", "wb");
    CHECK(bad != NULL);
    for (int i = 0; i < s_page_count; i++) {
        fwrite(s_pages[i], 1, s_page_lens[i], good);
        fwrite(s_pages[i], 1, s_page_lens[i], bad);
    }
    fclose(good);
    fclose(bad);
    if (bad != NULL) {
        FILE *patch = fopen("build/test_bad_page.ogg", "r+b");
        CHECK(patch != NULL);
        if (patch != NULL) {
            uint8_t flip = 0;
            fseek(patch, 22, SEEK_SET); // 首页 crc 字段首字节
            fread(&flip, 1, 1, patch);
            flip ^= 0xFF;
            fseek(patch, 22, SEEK_SET);
            fwrite(&flip, 1, 1, patch);
            fclose(patch);
        }
    }
}

int main(void)
{
    RUN_TEST(test_opus_head_content);
    RUN_TEST(test_opus_tags_content);
    RUN_TEST(test_basic_stream_structure);
    RUN_TEST(test_exact_255_packet_gets_zero_terminator);
    RUN_TEST(test_oversize_packet_spans_pages);
    RUN_TEST(test_multi_packet_single_page);
    RUN_TEST(test_empty_packet);
    RUN_TEST(test_flush_then_finalize);
    RUN_TEST(test_eos_via_packet_flag_finalize_idempotent);
    RUN_TEST(test_granule_regression_rejected);
    RUN_TEST(test_bos_second_packet_rejected);
    RUN_TEST(test_init_guard);
    RUN_TEST(test_e2e_write_file);
    return test_report();
}
