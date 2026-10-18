// FRAG 分片重组单测：§11 分片示例（500B 消息 3 片）+ 乱序/丢片/超上限/混合流/重复片。
// 帧经 buzz_parser 真实管线（编码→解析→重组）喂入，而非手工构造视图。
#include <stdint.h>
#include <string.h>

#include "buzz_proto.h"
#include "buzz_reasm.h"
#include "test.h"

#define MSG_LEN 500u
#define FRAG0_LEN 228u
#define FRAG1_LEN 228u
#define FRAG2_LEN (MSG_LEN - FRAG0_LEN - FRAG1_LEN) // 44

// 500B 测试消息（确定性 pattern）
static uint8_t s_msg[MSG_LEN];
static uint8_t s_buf[600];  // 重组缓冲
static uint8_t s_bmp[64];   // 512 片位图

// parser→reasm 管线：回调内同步喂重组器
typedef struct {
    buzz_reasm_t *reasm;
    const buzz_frame_t *last_msg;
    int last_rc;
    int delivered;
    int errors;
} pipe_t;

static void pipe_cb(const buzz_frame_t *frame, void *ctx)
{
    pipe_t *p = ctx;
    p->last_msg = NULL;
    p->last_rc = buzz_reasm_feed(p->reasm, frame, &p->last_msg);
    if (p->last_rc != BUZZ_OK) {
        p->errors++;
    }
    if (p->last_msg != NULL) {
        p->delivered++;
    }
}

// 编码一片并经 parser 管线送出
static void send_frag(buzz_parser_t *parser, pipe_t *p, uint8_t type, uint8_t session,
                      uint16_t channel, uint16_t idx, uint16_t cnt, const uint8_t *data,
                      uint16_t len)
{
    uint8_t bytes[BUZZ_FRAME_HEADER_LEN + BUZZ_FRAME_MAX_PAYLOAD + BUZZ_FRAME_CRC_LEN];
    int n = buzz_frag_encode(bytes, sizeof(bytes), type, session, channel, idx, cnt,
                             data, len);
    CHECK(n > 0);
    CHECK_EQ_INT(buzz_parser_feed(parser, bytes, (size_t)n), BUZZ_OK);
    (void)p;
}

// 按自定义顺序发送 3 片示例消息
static void send_by_order(buzz_parser_t *parser, pipe_t *p, const uint16_t *order,
                          size_t n)
{
    static const uint16_t lens[3] = {FRAG0_LEN, FRAG1_LEN, FRAG2_LEN};
    for (size_t i = 0; i < n; i++) {
        uint16_t idx = order[i];
        send_frag(parser, p, BUZZ_TYPE_LIST_RESP, 0x2b, BUZZ_CHANNEL_CONTROL, idx, 3,
                  s_msg + (uint32_t)idx * FRAG0_LEN, lens[idx]);
    }
}

static void setup(buzz_reasm_t *reasm, buzz_parser_t *parser, pipe_t *p)
{
    for (uint32_t i = 0; i < MSG_LEN; i++) {
        s_msg[i] = (uint8_t)(i * 7u + 3u);
    }
    buzz_reasm_init(reasm, s_buf, sizeof(s_buf), s_bmp, 512);
    p->reasm = reasm;
    p->last_msg = NULL;
    p->last_rc = BUZZ_OK;
    p->delivered = 0;
    p->errors = 0;
    buzz_parser_init(parser, pipe_cb, p);
}

static void check_msg(const buzz_frame_t *msg)
{
    CHECK(msg != NULL);
    if (msg == NULL) {
        return;
    }
    CHECK_EQ_INT(msg->type, BUZZ_TYPE_LIST_RESP);
    CHECK_EQ_INT(msg->session, 0x2b);
    CHECK_EQ_INT(msg->channel, BUZZ_CHANNEL_CONTROL);
    CHECK_EQ_INT(msg->flags, 0); // FRAG 已消解
    CHECK_EQ_INT(msg->length, MSG_LEN);
    CHECK(memcmp(msg->payload, s_msg, MSG_LEN) == 0);
}

static void test_in_order(void)
{
    buzz_reasm_t r;
    buzz_parser_t p;
    pipe_t pipe;
    setup(&r, &p, &pipe);
    static const uint16_t order[3] = {0, 1, 2};
    send_by_order(&p, &pipe, order, 3);
    CHECK_EQ_INT(pipe.errors, 0);
    CHECK_EQ_INT(pipe.delivered, 1);
    check_msg(pipe.last_msg);
}

static void test_out_of_order(void)
{
    buzz_reasm_t r;
    buzz_parser_t p;
    pipe_t pipe;
    setup(&r, &p, &pipe);
    static const uint16_t order[3] = {1, 2, 0}; // 末片先于片 0
    send_by_order(&p, &pipe, order, 3);
    CHECK_EQ_INT(pipe.errors, 0);
    CHECK_EQ_INT(pipe.delivered, 1);
    check_msg(pipe.last_msg);
}

static void test_last_fragment_first(void)
{
    buzz_reasm_t r;
    buzz_parser_t p;
    pipe_t pipe;
    setup(&r, &p, &pipe);
    static const uint16_t order[3] = {2, 1, 0}; // stride 未定时末片先到（暂存路径）
    send_by_order(&p, &pipe, order, 3);
    CHECK_EQ_INT(pipe.errors, 0);
    CHECK_EQ_INT(pipe.delivered, 1);
    check_msg(pipe.last_msg);
}

static void test_missing_fragment_no_delivery(void)
{
    buzz_reasm_t r;
    buzz_parser_t p;
    pipe_t pipe;
    setup(&r, &p, &pipe);
    static const uint16_t order[2] = {0, 1}; // 缺片 2
    send_by_order(&p, &pipe, order, 2);
    CHECK_EQ_INT(pipe.errors, 0);
    CHECK_EQ_INT(pipe.delivered, 0);
}

static void test_duplicate_fragment_idempotent(void)
{
    buzz_reasm_t r;
    buzz_parser_t p;
    pipe_t pipe;
    setup(&r, &p, &pipe);
    static const uint16_t order[5] = {0, 0, 1, 1, 2}; // 片 0/1 各重复一次
    send_by_order(&p, &pipe, order, 5);
    CHECK_EQ_INT(pipe.errors, 0);
    CHECK_EQ_INT(pipe.delivered, 1); // 不因重复片提前交付
    check_msg(pipe.last_msg);
}

static void test_mixed_frag_and_plain_stream(void)
{
    buzz_reasm_t r;
    buzz_parser_t p;
    pipe_t pipe;
    setup(&r, &p, &pipe);
    static const uint16_t first[1] = {0};
    send_by_order(&p, &pipe, first, 1);
    CHECK_EQ_INT(pipe.delivered, 0);

    // 分片响应中途插入链路通道 PING（非 FRAG）：应透传且不打断重组
    uint8_t ping[BUZZ_FRAME_HEADER_LEN + BUZZ_FRAME_CRC_LEN];
    int n = buzz_frame_encode(ping, sizeof(ping), BUZZ_TYPE_PING, 0x00, 0,
                              BUZZ_CHANNEL_LINK, NULL, 0);
    CHECK(n > 0);
    CHECK_EQ_INT(buzz_parser_feed(&p, ping, (size_t)n), BUZZ_OK);
    CHECK_EQ_INT(pipe.errors, 0);
    CHECK_EQ_INT(pipe.delivered, 1); // PING 透传即"交付"
    CHECK_EQ_INT(pipe.last_msg->type, BUZZ_TYPE_PING);

    static const uint16_t rest[2] = {1, 2};
    send_by_order(&p, &pipe, rest, 2);
    CHECK_EQ_INT(pipe.errors, 0);
    CHECK_EQ_INT(pipe.delivered, 2); // PING + 重组消息
    check_msg(pipe.last_msg);
}

static void test_over_64k_rejected(void)
{
    buzz_reasm_t r;
    buzz_parser_t p;
    pipe_t pipe;
    setup(&r, &p, &pipe);
    // 228 × 289 = 65892 > 64 KB 硬顶：首片即拒绝
    uint8_t big[BUZZ_FRAG_MAX_DATA] = {0};
    send_frag(&p, &pipe, BUZZ_TYPE_LIST_RESP, 0x2b, BUZZ_CHANNEL_CONTROL, 0, 289, big,
              BUZZ_FRAG_MAX_DATA);
    CHECK_EQ_INT(pipe.errors, 1);
    CHECK_EQ_INT(pipe.last_rc, BUZZ_ERR_PROTO);
    CHECK_EQ_INT(pipe.delivered, 0);
}

static void test_over_caller_buffer_rejected(void)
{
    buzz_reasm_t r;
    buzz_parser_t p;
    pipe_t pipe;
    buzz_reasm_init(&r, s_buf, 400, s_bmp, 512); // 缓冲小于 500B 消息
    pipe.reasm = &r;
    pipe.last_msg = NULL;
    pipe.last_rc = BUZZ_OK;
    pipe.delivered = 0;
    pipe.errors = 0;
    for (uint32_t i = 0; i < MSG_LEN; i++) {
        s_msg[i] = (uint8_t)(i * 7u + 3u);
    }
    buzz_parser_init(&p, pipe_cb, &pipe);

    static const uint16_t order[2] = {0, 1}; // 片 1 落位时 456 > 400
    send_by_order(&p, &pipe, order, 2);
    CHECK(pipe.errors >= 1);
    CHECK_EQ_INT(pipe.last_rc, BUZZ_ERR_SIZE);
}

static void test_session_mismatch_rejected(void)
{
    buzz_reasm_t r;
    buzz_parser_t p;
    pipe_t pipe;
    setup(&r, &p, &pipe);
    send_frag(&p, &pipe, BUZZ_TYPE_LIST_RESP, 0x2b, BUZZ_CHANNEL_CONTROL, 0, 3, s_msg,
              FRAG0_LEN);
    CHECK_EQ_INT(pipe.errors, 0);
    // 同 type/channel 但 session 不同：不属于同一消息
    send_frag(&p, &pipe, BUZZ_TYPE_LIST_RESP, 0x2c, BUZZ_CHANNEL_CONTROL, 1, 3,
              s_msg + FRAG0_LEN, FRAG1_LEN);
    CHECK_EQ_INT(pipe.errors, 1);
    CHECK_EQ_INT(pipe.last_rc, BUZZ_ERR_PROTO);
}

int main(void)
{
    RUN_TEST(test_in_order);
    RUN_TEST(test_out_of_order);
    RUN_TEST(test_last_fragment_first);
    RUN_TEST(test_missing_fragment_no_delivery);
    RUN_TEST(test_duplicate_fragment_idempotent);
    RUN_TEST(test_mixed_frag_and_plain_stream);
    RUN_TEST(test_over_64k_rejected);
    RUN_TEST(test_over_caller_buffer_rejected);
    RUN_TEST(test_session_mismatch_rejected);
    return test_report();
}
