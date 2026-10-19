// WAV 封装写入器单测：头字段逐字节断言（大小端/格式常量）+ /tmp 真实落盘
// 复读校验（占位头 → 追加 PCM → close 回填长度 → 重新打开解析）。
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"
#include "wav_writer.h"

#define TEST_WAV_PATH "/tmp/test_buzz_wav_writer.wav"
#define PCM_N_BYTES 4096u // 有意取 4 的倍数之外也行；此处为块对齐 2 的整数倍即可

// ---------- 头字段读取（测试侧独立小端解码） ----------

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// ---------- 头生成纯函数 ----------

static void test_header_content_and_endianness(void)
{
    uint8_t h[BUZZ_WAV_HEADER_LEN];

    CHECK_EQ_INT(buzz_wav_build_header(h, sizeof(h), 0u), (int)BUZZ_WAV_HEADER_LEN);
    // 魔数与块标签
    CHECK(memcmp(h + 0, "RIFF", 4) == 0);
    CHECK(memcmp(h + 8, "WAVE", 4) == 0);
    CHECK(memcmp(h + 12, "fmt ", 4) == 0);
    CHECK(memcmp(h + 36, "data", 4) == 0);
    // fmt 块：PCM / 单声道 / 16kHz / 16bit，字节率 32000，块对齐 2
    CHECK_EQ_INT(rd_le32(h + 16), 16u);
    CHECK_EQ_INT(rd_le16(h + 20), 1u);
    CHECK_EQ_INT(rd_le16(h + 22), 1u);
    CHECK_EQ_INT(rd_le32(h + 24), 16000u);
    CHECK_EQ_INT(rd_le32(h + 28), 32000u);
    CHECK_EQ_INT(rd_le16(h + 32), 2u);
    CHECK_EQ_INT(rd_le16(h + 34), 16u);
    // 小端序显式断言：16000 = 0x3E80、32000 = 0x7D00 的低字节在前
    CHECK_EQ_INT(h[24], 0x80);
    CHECK_EQ_INT(h[25], 0x3E);
    CHECK_EQ_INT(h[28], 0x00);
    CHECK_EQ_INT(h[29], 0x7D);
    // 零数据：RIFF chunk size = 36，data size = 0
    CHECK_EQ_INT(rd_le32(h + 4), 36u);
    CHECK_EQ_INT(rd_le32(h + 40), 0u);

    // 非零数据：N 字节 → chunk size = 36+N，data size = N
    CHECK_EQ_INT(buzz_wav_build_header(h, sizeof(h), PCM_N_BYTES), (int)BUZZ_WAV_HEADER_LEN);
    CHECK_EQ_INT(rd_le32(h + 4), 36u + PCM_N_BYTES);
    CHECK_EQ_INT(rd_le32(h + 40), PCM_N_BYTES);

    // 参数守卫
    CHECK_EQ_INT(buzz_wav_build_header(NULL, sizeof(h), 0u), BUZZ_ERR_NULL);
    CHECK_EQ_INT(buzz_wav_build_header(h, 10u, 0u), BUZZ_ERR_SIZE);
}

// ---------- 文件薄封装：占位头 / 追加 / 回填 / 复读 ----------

static void fill_pattern(uint8_t *pcm, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        pcm[i] = (uint8_t)(i * 7u + 3u); // 与块内容无关的确定性伪数据
    }
}

static void test_file_roundtrip(void)
{
    uint8_t pcm[PCM_N_BYTES];
    fill_pattern(pcm, sizeof(pcm));

    buzz_wav_writer_t w;
    uint8_t raw[BUZZ_WAV_HEADER_LEN + PCM_N_BYTES];
    CHECK_EQ_INT(buzz_wav_open(&w, TEST_WAV_PATH), BUZZ_OK);
    CHECK(w.fp != NULL);

    // 占位头：close 前 RIFF/data 长度字段为 0（先验，防止实现提前回填）。
    // fflush 后从另一个句柄读同一路径（w.fp 是 "wb"，不可直接读）。
    fflush(w.fp);
    FILE *probe = fopen(TEST_WAV_PATH, "rb");
    CHECK(probe != NULL);
    if (probe != NULL) {
        CHECK_EQ_INT(fread(raw, 1, BUZZ_WAV_HEADER_LEN, probe), BUZZ_WAV_HEADER_LEN);
        fclose(probe);
    }
    CHECK_EQ_INT(rd_le32(raw + 4), 36u);
    CHECK_EQ_INT(rd_le32(raw + 40), 0u);
    // 占位头其余字段已是终值
    CHECK(memcmp(raw + 0, "RIFF", 4) == 0);
    CHECK_EQ_INT(rd_le32(raw + 24), 16000u);
    CHECK_EQ_INT(rd_le16(raw + 34), 16u);

    // 分两次追加（模拟采集循环多块写入），计数累计
    CHECK_EQ_INT(buzz_wav_write(&w, pcm, 1000u), BUZZ_OK);
    CHECK_EQ_INT(buzz_wav_write(&w, pcm + 1000u, sizeof(pcm) - 1000u), BUZZ_OK);
    CHECK_EQ_INT((long)w.data_bytes, (long)sizeof(pcm));

    CHECK_EQ_INT(buzz_wav_close(&w), BUZZ_OK);
    CHECK(w.fp == NULL);

    // 复读校验：头字段 + PCM 内容
    FILE *f = fopen(TEST_WAV_PATH, "rb");
    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    CHECK_EQ_INT(fread(raw, 1, sizeof(raw), f), sizeof(raw));
    fclose(f);

    CHECK(memcmp(raw + 0, "RIFF", 4) == 0);
    CHECK_EQ_INT(rd_le32(raw + 4), 36u + PCM_N_BYTES); // RIFF size = 36+N
    CHECK(memcmp(raw + 8, "WAVE", 4) == 0);
    CHECK(memcmp(raw + 12, "fmt ", 4) == 0);
    CHECK_EQ_INT(rd_le32(raw + 16), 16u);
    CHECK_EQ_INT(rd_le16(raw + 20), 1u);
    CHECK_EQ_INT(rd_le16(raw + 22), 1u);
    CHECK_EQ_INT(rd_le32(raw + 24), 16000u);
    CHECK_EQ_INT(rd_le32(raw + 28), 32000u);
    CHECK_EQ_INT(rd_le16(raw + 32), 2u);
    CHECK_EQ_INT(rd_le16(raw + 34), 16u);
    CHECK(memcmp(raw + 36, "data", 4) == 0);
    CHECK_EQ_INT(rd_le32(raw + 40), PCM_N_BYTES); // data size = N
    CHECK(memcmp(raw + BUZZ_WAV_HEADER_LEN, pcm, sizeof(pcm)) == 0);

    remove(TEST_WAV_PATH);
}

static void test_empty_file_roundtrip(void)
{
    // 零 PCM：合法 WAV（长度 44），头字段全零数据
    buzz_wav_writer_t w;
    CHECK_EQ_INT(buzz_wav_open(&w, TEST_WAV_PATH), BUZZ_OK);
    CHECK_EQ_INT(buzz_wav_close(&w), BUZZ_OK);

    FILE *f = fopen(TEST_WAV_PATH, "rb");
    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    uint8_t h[BUZZ_WAV_HEADER_LEN];
    CHECK_EQ_INT(fread(h, 1, sizeof(h), f), BUZZ_WAV_HEADER_LEN);
    CHECK_EQ_INT(fgetc(f), EOF); // 文件恰好 44 字节
    fclose(f);
    CHECK_EQ_INT(rd_le32(h + 4), 36u);
    CHECK_EQ_INT(rd_le32(h + 40), 0u);
    remove(TEST_WAV_PATH);
}

static void test_api_guards(void)
{
    buzz_wav_writer_t w;
    uint8_t one[2] = {0, 0};

    CHECK_EQ_INT(buzz_wav_open(NULL, TEST_WAV_PATH), BUZZ_ERR_NULL);
    CHECK_EQ_INT(buzz_wav_open(&w, NULL), BUZZ_ERR_NULL);

    // 未打开即写 / 空指针写
    memset(&w, 0, sizeof(w));
    CHECK_EQ_INT(buzz_wav_write(&w, one, sizeof(one)), BUZZ_ERR_NULL);
    CHECK_EQ_INT(buzz_wav_write(NULL, one, sizeof(one)), BUZZ_ERR_NULL);
    CHECK_EQ_INT(buzz_wav_write(&w, NULL, 4u), BUZZ_ERR_NULL);

    // close 幂等：未打开、重复关闭都返回 BUZZ_OK
    CHECK_EQ_INT(buzz_wav_close(&w), BUZZ_OK);
    CHECK_EQ_INT(buzz_wav_close(NULL), BUZZ_ERR_NULL);
    CHECK_EQ_INT(buzz_wav_open(&w, TEST_WAV_PATH), BUZZ_OK);
    CHECK_EQ_INT(buzz_wav_close(&w), BUZZ_OK);
    CHECK_EQ_INT(buzz_wav_close(&w), BUZZ_OK);
    remove(TEST_WAV_PATH);
}

int main(void)
{
    RUN_TEST(test_header_content_and_endianness);
    RUN_TEST(test_file_roundtrip);
    RUN_TEST(test_empty_file_roundtrip);
    RUN_TEST(test_api_guards);
    return test_report();
}
