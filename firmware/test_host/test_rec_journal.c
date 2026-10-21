// rec_journal 宿主单测（M5.3）：序列化往返、校验拒绝、恢复决策、WAV 头回填。
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../main/audio/rec_journal.h"
#include "../main/proto/crc16.h"
#include "test.h"

static rec_journal_t make_journal(void)
{
    rec_journal_t j;
    memset(&j, 0, sizeof(j));
    snprintf(j.file, sizeof(j.file), "%s", "/REC_0021.OGG");
    j.fmt = 0;
    j.bytes = 12345678ull;
    j.elapsed_ms = 60000u;
    j.marks = 3u;
    j.mark_ms[0] = 5000u;
    j.mark_ms[1] = 21000u;
    j.mark_ms[2] = 47000u;
    return j;
}

static void test_encode_decode_roundtrip(void)
{
    rec_journal_t j = make_journal();
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    int len = rec_journal_encode(&j, buf, sizeof(buf));
    CHECK(len == (int)REC_JOURNAL_ENCODED_SIZE);

    rec_journal_t out;
    CHECK(rec_journal_decode(buf, (uint32_t)len, &out) == 0);
    CHECK(strcmp(out.file, j.file) == 0);
    CHECK(out.fmt == j.fmt);
    CHECK(out.bytes == j.bytes);
    CHECK(out.elapsed_ms == j.elapsed_ms);
    CHECK(out.marks == j.marks);
    CHECK(out.mark_ms[0] == 5000u && out.mark_ms[1] == 21000u && out.mark_ms[2] == 47000u);
}

static void test_encoded_size_constant(void)
{
    CHECK(REC_JOURNAL_ENCODED_SIZE == 8u + 64u + 4u + 8u + 4u + 4u + 64u * 4u + 2u);
    rec_journal_t j = make_journal();
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    CHECK(rec_journal_encode(&j, buf, sizeof(buf)) == (int)sizeof(buf));
}

static void test_buffer_too_small_rejected(void)
{
    rec_journal_t j = make_journal();
    uint8_t small[REC_JOURNAL_ENCODED_SIZE - 1];
    CHECK(rec_journal_encode(&j, small, sizeof(small)) == -1);
}

static void test_bad_magic_rejected(void)
{
    rec_journal_t j = make_journal();
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    rec_journal_encode(&j, buf, sizeof(buf));
    buf[0] ^= 0xFFu;
    rec_journal_t out;
    CHECK(rec_journal_decode(buf, sizeof(buf), &out) == -1);
}

static void test_bad_crc_rejected(void)
{
    rec_journal_t j = make_journal();
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    rec_journal_encode(&j, buf, sizeof(buf));
    buf[REC_JOURNAL_ENCODED_SIZE - 3] ^= 0x01u; //  CRC 前一字节
    rec_journal_t out;
    CHECK(rec_journal_decode(buf, sizeof(buf), &out) == -1);
}

static void test_truncated_rejected(void)
{
    rec_journal_t j = make_journal();
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    rec_journal_encode(&j, buf, sizeof(buf));
    rec_journal_t out;
    CHECK(rec_journal_decode(buf, sizeof(buf) - 1, &out) == -1);
}

static void test_long_filename_forced_terminated(void)
{
    rec_journal_t j = make_journal();
    memset(j.file, 'A', sizeof(j.file)); // 无 NUL：满 64 字节非终止串
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    rec_journal_encode(&j, buf, sizeof(buf));
    rec_journal_t out;
    CHECK(rec_journal_decode(buf, sizeof(buf), &out) == 0);
    CHECK(out.file[REC_JOURNAL_FILE_MAX - 1] == '\0');
}

static void test_plan_file_larger_truncates(void)
{
    rec_journal_t j = make_journal();
    j.bytes = 1000u;
    uint64_t to = 0;
    CHECK(rec_journal_plan_recovery(&j, 1500u, &to));
    CHECK(to == 1000u); // 尾部 500 字节撕毁，截到已确认
}

static void test_plan_file_equal_keeps(void)
{
    rec_journal_t j = make_journal();
    j.bytes = 1000u;
    uint64_t to = 0;
    CHECK(rec_journal_plan_recovery(&j, 1000u, &to));
    CHECK(to == 1000u);
}

static void test_plan_file_smaller_keeps_actual(void)
{
    rec_journal_t j = make_journal();
    j.bytes = 2000u; // journal 超前（写完 journal 后还没写那么多数据就断电）
    uint64_t to = 0;
    CHECK(rec_journal_plan_recovery(&j, 800u, &to));
    CHECK(to == 800u); // 以实际为准，不凭空扩
}

static void test_plan_missing_file_rejected(void)
{
    rec_journal_t j = make_journal();
    uint64_t to = 0;
    CHECK(!rec_journal_plan_recovery(&j, 0u, &to)); // actual=0 → 文件不存在
}

static void test_plan_null_args(void)
{
    rec_journal_t j = make_journal();
    uint64_t to = 0;
    CHECK(!rec_journal_plan_recovery(NULL, 100u, &to));
    CHECK(!rec_journal_plan_recovery(&j, 100u, NULL));
}

static void test_marks_roundtrip_full(void)
{
    rec_journal_t j = make_journal();
    j.marks = REC_JOURNAL_MARKS_MAX;
    for (uint32_t i = 0; i < REC_JOURNAL_MARKS_MAX; i++) {
        j.mark_ms[i] = i * 1000u;
    }
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    rec_journal_encode(&j, buf, sizeof(buf));
    rec_journal_t out;
    CHECK(rec_journal_decode(buf, sizeof(buf), &out) == 0);
    CHECK(out.marks == REC_JOURNAL_MARKS_MAX);
    CHECK(out.mark_ms[63] == 63000u);
}

static void test_marks_beyond_max_rejected(void)
{
    rec_journal_t j = make_journal();
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    rec_journal_encode(&j, buf, sizeof(buf));
    // 手工把 marks 字段改坏（偏移：8+64+8+8 = 88 起 4 字节小端）
    const uint32_t marks_off = 8u + 64u + 8u + 8u;
    buf[marks_off] = (uint8_t)((REC_JOURNAL_MARKS_MAX + 1) & 0xFFu);
    buf[marks_off + 1] = (uint8_t)(((REC_JOURNAL_MARKS_MAX + 1) >> 8) & 0xFFu);
    // CRC 会拦下这次篡改（buf 其余字节未动，CRC 不匹配）
    rec_journal_t out;
    CHECK(rec_journal_decode(buf, sizeof(buf), &out) == -1);
    (void)j;
}

static void test_fmt_wav_accepted(void)
{
    rec_journal_t j = make_journal();
    j.fmt = 1u;
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    rec_journal_encode(&j, buf, sizeof(buf));
    rec_journal_t out;
    CHECK(rec_journal_decode(buf, sizeof(buf), &out) == 0);
    CHECK(out.fmt == 1u);
}

static void test_wav_patch_header_layout(void)
{
    // RIFF/WAVE 规范头：riff_size@4，data_size@40
    uint8_t hdr[44];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 8, "WAVE", 4);
    rec_journal_wav_patch_header(hdr, 1000u);
    CHECK(hdr[4] == (uint8_t)((1000u + 36u) & 0xFFu));
    CHECK(hdr[5] == (uint8_t)(((1000u + 36u) >> 8) & 0xFFu));
    CHECK(hdr[40] == (uint8_t)(1000u & 0xFFu));
    CHECK(hdr[41] == (uint8_t)((1000u >> 8) & 0xFFu));
    CHECK(hdr[42] == 0u && hdr[43] == 0u);
    // 其余字节不动
    CHECK(memcmp(hdr, "RIFF", 4) == 0);
    CHECK(memcmp(hdr + 8, "WAVE", 4) == 0);
}

static void test_wav_patch_null_safe(void)
{
    rec_journal_wav_patch_header(NULL, 100u); // 不崩即可
}

static void test_zero_session_fields(void)
{
    // 空会话（0 字节、0 标记）往返
    rec_journal_t j;
    memset(&j, 0, sizeof(j));
    snprintf(j.file, sizeof(j.file), "%s", "/REC_0001.OGG");
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    rec_journal_encode(&j, buf, sizeof(buf));
    rec_journal_t out;
    CHECK(rec_journal_decode(buf, sizeof(buf), &out) == 0);
    CHECK(out.bytes == 0u && out.marks == 0u && out.elapsed_ms == 0u);
}

static void test_deterministic_encoding(void)
{
    rec_journal_t j = make_journal();
    uint8_t a[REC_JOURNAL_ENCODED_SIZE];
    uint8_t b[REC_JOURNAL_ENCODED_SIZE];
    CHECK(rec_journal_encode(&j, a, sizeof(a)) == (int)sizeof(a));
    CHECK(rec_journal_encode(&j, b, sizeof(b)) == (int)sizeof(b));
    CHECK(memcmp(a, b, sizeof(a)) == 0);
}

static void test_two_journals_independent(void)
{
    rec_journal_t j1 = make_journal();
    rec_journal_t j2 = make_journal();
    snprintf(j2.file, sizeof(j2.file), "%s", "/REC_0099.WAV");
    j2.fmt = 1u;
    j2.bytes = 999u;
    uint8_t b1[REC_JOURNAL_ENCODED_SIZE];
    uint8_t b2[REC_JOURNAL_ENCODED_SIZE];
    rec_journal_encode(&j1, b1, sizeof(b1));
    rec_journal_encode(&j2, b2, sizeof(b2));
    rec_journal_t o1, o2;
    CHECK(rec_journal_decode(b1, sizeof(b1), &o1) == 0);
    CHECK(rec_journal_decode(b2, sizeof(b2), &o2) == 0);
    CHECK(strcmp(o1.file, "/REC_0021.OGG") == 0);
    CHECK(strcmp(o2.file, "/REC_0099.WAV") == 0);
    CHECK(o2.fmt == 1u && o2.bytes == 999u);
    CHECK(o1.bytes == j1.bytes);
}

static void test_len_larger_than_struct_accepted(void)
{
    // 契约：len ≥ 定长即可解析（调用方负责查尾部多余字节）
    rec_journal_t j = make_journal();
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE + 8];
    memset(buf, 0xAA, sizeof(buf));
    rec_journal_encode(&j, buf, REC_JOURNAL_ENCODED_SIZE);
    rec_journal_t out;
    CHECK(rec_journal_decode(buf, sizeof(buf), &out) == 0);
    CHECK(out.bytes == j.bytes);
}

static void test_fmt_out_of_range_rejected(void)
{
    // 篡改 fmt=2 并用正确 CRC 重签（绕过 CRC 拦截，验证语义校验）
    rec_journal_t j = make_journal();
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    rec_journal_encode(&j, buf, sizeof(buf));
    const uint32_t fmt_off = 8u + 64u;
    buf[fmt_off] = 2u;
    const uint16_t crc = buzz_crc16(buf, REC_JOURNAL_ENCODED_SIZE - 2u);
    buf[REC_JOURNAL_ENCODED_SIZE - 2u] = (uint8_t)(crc & 0xFFu);
    buf[REC_JOURNAL_ENCODED_SIZE - 1u] = (uint8_t)(crc >> 8);
    rec_journal_t out;
    CHECK(rec_journal_decode(buf, sizeof(buf), &out) == -1);
}

static void test_reserved_bytes_roundtrip(void)
{
    rec_journal_t j = make_journal();
    j.reserved[0] = 0x12u;
    j.reserved[1] = 0x34u;
    j.reserved[2] = 0x56u;
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    rec_journal_encode(&j, buf, sizeof(buf));
    rec_journal_t out;
    CHECK(rec_journal_decode(buf, sizeof(buf), &out) == 0);
    CHECK(out.reserved[0] == 0x12u && out.reserved[1] == 0x34u && out.reserved[2] == 0x56u);
}

static void test_filename_exact_fit(void)
{
    rec_journal_t j = make_journal();
    // 63 字符 + NUL 恰好装下
    memset(j.file, 'f', sizeof(j.file) - 1u);
    j.file[sizeof(j.file) - 1u] = '\0';
    uint8_t buf[REC_JOURNAL_ENCODED_SIZE];
    rec_journal_encode(&j, buf, sizeof(buf));
    rec_journal_t out;
    CHECK(rec_journal_decode(buf, sizeof(buf), &out) == 0);
    CHECK(strlen(out.file) == REC_JOURNAL_FILE_MAX - 1u);
}

static void test_plan_truncates_to_small_confirmed_size(void)
{
    // 极端：journal 只确认到 100 字节（刚写完头几页就断电），大文件截到 100
    rec_journal_t j = make_journal();
    j.bytes = 100u;
    uint64_t to = 0;
    CHECK(rec_journal_plan_recovery(&j, 5000u, &to));
    CHECK(to == 100u);
}

int main(void)
{
    RUN_TEST(test_encode_decode_roundtrip);
    RUN_TEST(test_encoded_size_constant);
    RUN_TEST(test_buffer_too_small_rejected);
    RUN_TEST(test_bad_magic_rejected);
    RUN_TEST(test_bad_crc_rejected);
    RUN_TEST(test_truncated_rejected);
    RUN_TEST(test_long_filename_forced_terminated);
    RUN_TEST(test_plan_file_larger_truncates);
    RUN_TEST(test_plan_file_equal_keeps);
    RUN_TEST(test_plan_file_smaller_keeps_actual);
    RUN_TEST(test_plan_missing_file_rejected);
    RUN_TEST(test_plan_null_args);
    RUN_TEST(test_marks_roundtrip_full);
    RUN_TEST(test_marks_beyond_max_rejected);
    RUN_TEST(test_fmt_wav_accepted);
    RUN_TEST(test_wav_patch_header_layout);
    RUN_TEST(test_wav_patch_null_safe);
    RUN_TEST(test_zero_session_fields);
    RUN_TEST(test_deterministic_encoding);
    RUN_TEST(test_two_journals_independent);
    RUN_TEST(test_len_larger_than_struct_accepted);
    RUN_TEST(test_fmt_out_of_range_rejected);
    RUN_TEST(test_reserved_bytes_roundtrip);
    RUN_TEST(test_filename_exact_fit);
    RUN_TEST(test_plan_truncates_to_small_confirmed_size);
    return test_report();
}
