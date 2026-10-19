// rec_pipe 全链路 roundtrip 单测（M3）：合成 PCM（扫频 + 静音 + 多频叠加）→
// rec_pipe → build/test_pipe.ogg；手写极简 Ogg 页解析把音频包挑出（顺便验证页结构），
// 真 libopus 48kHz 逐包解码校验：包数=帧数、每包 960 采样、静音段能量远低于发声段；
// granule/结构断言：末页 granule=960×帧数、首页 BOS 独占只含 OpusHead、EOS 页存在；
// 尾帧丢弃用例：喂 320×N+159 采样，文件帧数=N。空会话（零帧 finish）补 EOS 页。
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus.h>

#include "rec_pipe.h"
#include "test.h"

#define PIPE_OGG_PATH "build/test_pipe.ogg"
#define EMPTY_OGG_PATH "build/test_pipe_empty.ogg"
#define TAIL_OGG_PATH "build/test_pipe_tail.ogg"

// 合成信号：1s 扫频 + 0.5s 静音 + 1s 多频叠加 = 40000 采样
#define SYNTH_PI 3.14159265358979323846 // -std=c11 严格模式不保证 SYNTH_PI，自定义
#define SYNTH_CHIRP_SAMPLES 16000u
#define SYNTH_SILENCE_SAMPLES 8000u
#define SYNTH_TONE_SAMPLES 16000u
#define SYNTH_TOTAL (SYNTH_CHIRP_SAMPLES + SYNTH_SILENCE_SAMPLES + SYNTH_TONE_SAMPLES)
#define SYNTH_FRAMES (SYNTH_TOTAL / BUZZ_OPUS_ENC_FRAME_SAMPLES) // 125 整除

// ---------- 页接收 sink ----------

typedef struct {
    FILE *fp;
    uint32_t write_errs;
} sink_t;

static void sink_page_cb(const uint8_t *page, uint32_t page_len, void *ctx)
{
    sink_t *sink = (sink_t *)ctx;
    if (fwrite(page, 1, page_len, sink->fp) != page_len) {
        sink->write_errs++;
    }
}

// ---------- 极简 Ogg 页解析（测试侧独立实现，不依赖固件代码） ----------

#define MAX_PACKETS 512u
#define PKT_BUF_CAP (64u * 1024u)

typedef struct {
    uint8_t type;
    uint64_t granule;
    uint32_t serial;
    uint32_t seq;
    uint32_t payload_len;
} page_info_t;

static page_info_t g_pages[256];
static uint32_t g_npages;
static uint8_t g_pkt_buf[PKT_BUF_CAP];
static uint32_t g_pkt_starts[MAX_PACKETS];
static uint32_t g_pkt_lens[MAX_PACKETS];
static uint32_t g_npkts;

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

// 解析整文件：逐页记录页信息，按 lacing 重组包（视图指向 g_pkt_buf）
static void parse_ogg(const uint8_t *data, size_t len)
{
    g_npages = 0;
    g_npkts = 0;
    uint32_t acc_start = 0;
    uint32_t acc_len = 0;
    size_t pos = 0;
    while (pos < len && g_npages < 256u) {
        CHECK_EQ_INT(len - pos >= 27u, 1);
        if (len - pos < 27u) {
            return;
        }
        const uint8_t *h = data + pos;
        CHECK(memcmp(h, "OggS", 4) == 0);
        CHECK_EQ_INT(h[4], 0);
        page_info_t *pi = &g_pages[g_npages];
        pi->type = h[5];
        pi->granule = rd_le64(h + 6);
        pi->serial = rd_le32(h + 14);
        pi->seq = rd_le32(h + 18);
        const uint8_t nsegs = h[26];
        const uint8_t *segs = data + pos + 27u;
        CHECK(len - pos - 27u >= nsegs);
        uint32_t payload_len = 0;
        for (uint8_t i = 0; i < nsegs; i++) {
            payload_len += segs[i];
        }
        pi->payload_len = payload_len;
        const uint8_t *payload = segs + nsegs;
        CHECK(len - pos - 27u - nsegs >= payload_len);
        if (len - pos - 27u - nsegs < payload_len) {
            return;
        }

        // 续传页不应出现（4KB 页装 ~28 个 ~110B 包，无跨页包）
        CHECK_EQ_INT(pi->type & 0x01u, 0u);
        uint32_t off = 0;
        for (uint8_t i = 0; i < nsegs; i++) {
            CHECK(acc_len + segs[i] <= PKT_BUF_CAP);
            memcpy(g_pkt_buf + acc_start + acc_len, payload + off, segs[i]);
            acc_len += segs[i];
            off += segs[i];
            if (segs[i] < 255u) { // 包终结
                CHECK(g_npkts < MAX_PACKETS);
                g_pkt_starts[g_npkts] = acc_start;
                g_pkt_lens[g_npkts] = acc_len;
                g_npkts++;
                acc_start += acc_len;
                acc_len = 0;
            }
        }
        g_npages++;
        pos += 27u + nsegs + payload_len;
    }
    CHECK_EQ_INT(pos, len); // 恰好消费完
}

// ---------- 合成 PCM ----------

static void synth_pcm(int16_t *pcm)
{
    // 1s 扫频 300→3000Hz（相位累加器，频率随时间线性爬升）
    double phase = 0.0;
    for (uint32_t i = 0; i < SYNTH_CHIRP_SAMPLES; i++) {
        const double freq = 300.0 + 2700.0 * (double)i / (double)SYNTH_CHIRP_SAMPLES;
        phase += 2.0 * SYNTH_PI * freq / 16000.0;
        pcm[i] = (int16_t)(0.5 * 32767.0 * sin(phase));
    }
    for (uint32_t i = 0; i < SYNTH_SILENCE_SAMPLES; i++) {
        pcm[SYNTH_CHIRP_SAMPLES + i] = 0;
    }
    // 1s 多频叠加（语音样：基频 + 谐波）
    for (uint32_t i = 0; i < SYNTH_TONE_SAMPLES; i++) {
        const double t = (double)i / 16000.0;
        const double v = 0.30 * sin(2.0 * SYNTH_PI * 220.0 * t) +
                         0.20 * sin(2.0 * SYNTH_PI * 440.0 * t) +
                         0.12 * sin(2.0 * SYNTH_PI * 1320.0 * t);
        pcm[SYNTH_CHIRP_SAMPLES + SYNTH_SILENCE_SAMPLES + i] = (int16_t)(v * 32767.0);
    }
}

// 驱动一次完整会话：init → 奇数块 feed → finish，返回 finish 结果
static int run_session(const char *path, buzz_rec_pipe_t *pipe, uint8_t *page_buf,
                       const int16_t *pcm, uint32_t nsamples, buzz_rec_pipe_stats_t *stats)
{
    FILE *fp = fopen(path, "wb");
    CHECK(fp != NULL);
    sink_t sink = { .fp = fp, .write_errs = 0 };
    int ret = buzz_rec_pipe_init(pipe, 0x00ECBE01u, page_buf,
                                 BUZZ_REC_PIPE_PAGE_BUF_SIZE, sink_page_cb, &sink);
    CHECK_EQ_INT(ret, BUZZ_OK);
    if (ret != BUZZ_OK) {
        fclose(fp);
        return ret;
    }
    // 641 采样奇数块：故意让帧边界与读块边界错位，覆盖跨块攒帧
    uint32_t pos = 0;
    while (pos < nsamples && ret == BUZZ_OK) {
        uint32_t take = nsamples - pos;
        if (take > 641u) {
            take = 641u;
        }
        ret = buzz_rec_pipe_feed(pipe, pcm + pos, take);
        pos += take;
    }
    CHECK_EQ_INT(sink.write_errs, 0u);
    int fret = buzz_rec_pipe_finish(pipe);
    CHECK_EQ_INT(fclose(fp) == 0, 1);
    *stats = pipe->stats;
    return ret == BUZZ_OK ? fret : ret;
}

// ---------- 用例 ----------

static void test_full_roundtrip(void)
{
    static int16_t pcm[SYNTH_TOTAL];
    static buzz_rec_pipe_t pipe;
    static uint8_t page_buf[BUZZ_REC_PIPE_PAGE_BUF_SIZE];
    synth_pcm(pcm);

    buzz_rec_pipe_stats_t stats;
    CHECK_EQ_INT(run_session(PIPE_OGG_PATH, &pipe, page_buf, pcm, SYNTH_TOTAL, &stats),
                 BUZZ_OK);

    // 统计：帧数 = 125，输入采样全计，输出字节 = 文件大小
    CHECK_EQ_INT(stats.frames, SYNTH_FRAMES);
    CHECK_EQ_INT((uint32_t)stats.samples_in, SYNTH_TOTAL);
    FILE *f = fopen(PIPE_OGG_PATH, "rb");
    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    static uint8_t file_buf[64u * 1024u];
    size_t file_len = fread(file_buf, 1, sizeof(file_buf), f);
    fclose(f);
    CHECK(file_len < sizeof(file_buf)); // 容量内完整读入
    CHECK_EQ_INT(stats.bytes_out, (uint64_t)file_len);

    parse_ogg(file_buf, file_len);

    // 页结构：首页 BOS 独占且只含 OpusHead（19B，granule=0）；次页 OpusTags；
    // 末页 EOS；serial 一致、seq 连续、granule 单调不减
    CHECK(g_npages >= 3u);
    CHECK_EQ_INT(g_pages[0].type, BUZZ_OGG_HEADER_BOS);
    CHECK_EQ_INT(g_pages[0].granule, 0u);
    CHECK_EQ_INT(g_pages[0].payload_len, BUZZ_OGG_OPUS_HEAD_LEN);
    CHECK_EQ_INT(g_npkts >= 2u, 1u);
    CHECK(memcmp(g_pkt_buf + g_pkt_starts[0], "OpusHead", 8) == 0);
    CHECK_EQ_INT(g_pkt_lens[0], BUZZ_OGG_OPUS_HEAD_LEN);
    CHECK(memcmp(g_pkt_buf + g_pkt_starts[1], "OpusTags", 8) == 0);
    CHECK_EQ_INT(g_pages[g_npages - 1u].type & BUZZ_OGG_HEADER_EOS,
                 BUZZ_OGG_HEADER_EOS);
    for (uint32_t i = 1; i < g_npages; i++) {
        CHECK_EQ_INT(g_pages[i].serial, g_pages[0].serial);
        CHECK_EQ_INT(g_pages[i].seq, i);
        CHECK(g_pages[i].granule >= g_pages[i - 1u].granule);
    }
    // 末页 granule = 960 × 帧数
    CHECK_EQ_INT((uint32_t)g_pages[g_npages - 1u].granule,
                 SYNTH_FRAMES * BUZZ_OPUS_ENC_GRANULE_STEP);

    // 包数 = 头两包 + 帧数
    CHECK_EQ_INT(g_npkts, 2u + SYNTH_FRAMES);

    // 真 libopus 逐包解码（48kHz：20ms 帧 → 每包恰 960 采样）
    int err = OPUS_OK;
    OpusDecoder *dec = opus_decoder_create(48000, 1, &err);
    CHECK(dec != NULL);
    CHECK_EQ_INT(err, OPUS_OK);
    if (dec == NULL) {
        return;
    }
    static double rms[MAX_PACKETS];
    opus_int16 out[5760];
    for (uint32_t i = 0; i + 2u < g_npkts; i++) {
        const int n = opus_decode(dec, g_pkt_buf + g_pkt_starts[i + 2u],
                                  (opus_int32)g_pkt_lens[i + 2u], out,
                                  5760, 0);
        CHECK_EQ_INT(n, 960);
        double acc = 0.0;
        for (int j = 0; j < n; j++) {
            acc += (double)out[j] * (double)out[j];
        }
        rms[i] = sqrt(acc / (double)n);
    }
    opus_decoder_destroy(dec);

    // 能量分段：解码域相对输入域前移 preskip 312（48kHz）≈104 采样（16kHz），
    // 包 k 对应输入 [320k+104, 320k+424)。静音段取包 52..71，发声段取 2..47 与 77..120
    double silent_max = 0.0;
    for (uint32_t k = 52; k <= 71; k++) {
        if (rms[k] > silent_max) {
            silent_max = rms[k];
        }
    }
    double active_min = 1e30;
    for (uint32_t k = 2; k <= 47; k++) {
        if (rms[k] < active_min) {
            active_min = rms[k];
        }
    }
    for (uint32_t k = 77; k <= 120; k++) {
        if (rms[k] < active_min) {
            active_min = rms[k];
        }
    }
    CHECK(active_min > 1000.0); // 发声段绝对能量（幅值 ~0.5 满量程）
    CHECK(active_min > 100.0 * silent_max); // 40dB 以上分段隔离
}

static void test_tail_drop(void)
{
    static int16_t pcm[320u * 10u + 159u];
    static buzz_rec_pipe_t pipe;
    static uint8_t page_buf[BUZZ_REC_PIPE_PAGE_BUF_SIZE];
    for (uint32_t i = 0; i < sizeof(pcm) / sizeof(pcm[0]); i++) {
        pcm[i] = (int16_t)(i % 977);
    }
    buzz_rec_pipe_stats_t stats;
    CHECK_EQ_INT(run_session(TAIL_OGG_PATH, &pipe, page_buf, pcm,
                             (uint32_t)(sizeof(pcm) / sizeof(pcm[0])), &stats),
                 BUZZ_OK);
    CHECK_EQ_INT(stats.frames, 10u);
    CHECK_EQ_INT((uint32_t)stats.samples_in, 320u * 10u + 159u); // 尾采样计入输入

    // finish 幂等：重复调用仍返回 OK
    CHECK_EQ_INT(buzz_rec_pipe_finish(&pipe), BUZZ_OK);

    FILE *f = fopen(TAIL_OGG_PATH, "rb");
    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    static uint8_t file_buf[16u * 1024u];
    size_t file_len = fread(file_buf, 1, sizeof(file_buf), f);
    fclose(f);
    parse_ogg(file_buf, file_len);
    CHECK_EQ_INT(g_npkts, 12u); // OpusHead + OpusTags + 10 帧
    CHECK_EQ_INT((uint32_t)g_pages[g_npages - 1u].granule, 10u * 960u);
    CHECK_EQ_INT(g_pages[g_npages - 1u].type & BUZZ_OGG_HEADER_EOS,
                 BUZZ_OGG_HEADER_EOS);
}

static void test_empty_session(void)
{
    static buzz_rec_pipe_t pipe;
    static uint8_t page_buf[BUZZ_REC_PIPE_PAGE_BUF_SIZE];
    buzz_rec_pipe_stats_t stats;
    CHECK_EQ_INT(run_session(EMPTY_OGG_PATH, &pipe, page_buf, NULL, 0u, &stats),
                 BUZZ_OK);
    CHECK_EQ_INT(stats.frames, 0u);
    CHECK_EQ_INT((uint32_t)stats.samples_in, 0u);

    FILE *f = fopen(EMPTY_OGG_PATH, "rb");
    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    static uint8_t file_buf[4u * 1024u];
    size_t file_len = fread(file_buf, 1, sizeof(file_buf), f);
    fclose(f);
    parse_ogg(file_buf, file_len);
    // 三页：OpusHead（BOS）/ OpusTags / 空 EOS 末页
    CHECK_EQ_INT(g_npages, 3u);
    CHECK_EQ_INT(g_npkts, 2u);
    CHECK_EQ_INT(g_pages[2].type, BUZZ_OGG_HEADER_EOS);
    CHECK_EQ_INT(g_pages[2].payload_len, 0u);
    CHECK_EQ_INT((uint32_t)g_pages[2].granule, 0u); // 无帧：granule 保持 0
}

static void test_api_guards(void)
{
    static buzz_rec_pipe_t pipe;
    static uint8_t page_buf[BUZZ_REC_PIPE_PAGE_BUF_SIZE];
    int16_t one[1] = { 0 };

    CHECK_EQ_INT(buzz_rec_pipe_init(NULL, 1u, page_buf, sizeof(page_buf), NULL, NULL),
                 BUZZ_ERR_NULL);
    CHECK_EQ_INT(buzz_rec_pipe_init(&pipe, 1u, NULL, sizeof(page_buf), NULL, NULL),
                 BUZZ_ERR_ARG);
    CHECK_EQ_INT(buzz_rec_pipe_init(&pipe, 1u, page_buf,
                                    BUZZ_OGG_PAGE_BUF_MIN - 1u, NULL, NULL),
                 BUZZ_ERR_ARG);

    CHECK_EQ_INT(buzz_rec_pipe_feed(NULL, one, 1u), BUZZ_ERR_NULL);
    CHECK_EQ_INT(buzz_rec_pipe_feed(&pipe, NULL, 1u), BUZZ_ERR_NULL);
    CHECK_EQ_INT(buzz_rec_pipe_finish(NULL), BUZZ_ERR_NULL);
}

int main(void)
{
    RUN_TEST(test_full_roundtrip);
    RUN_TEST(test_tail_drop);
    RUN_TEST(test_empty_session);
    RUN_TEST(test_api_guards);
    return test_report();
}
