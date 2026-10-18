// 会话向量回放（TESTING.md §3）：同一份 JSON 由 tools/vector2c.py 生成 C 表，
// 与 Mac 侧 XCTest 共同消费，锁定两端对协议的理解一致。
//
// tx 条目（Central→Device）：Python 独立封装的完整帧字节喂 C 解析器，
//   断言解码出的 version/type/session/channel/payload 与条目一致；
// rx 条目（Device→Central）：C 编码器按条目字段编码，
//   断言字节与 Python 独立封装的完整帧逐一相等（跨实现交叉验证）。
#include <stdint.h>
#include <string.h>

#include "buzz_proto.h"
#include "test.h"
#include "vectors_basic_session.h"

// 捕获最近一帧（payload 复制出来，避免视图失效）
typedef struct {
    int count;
    buzz_frame_t frame;
    uint8_t payload[BUZZ_FRAME_MAX_PAYLOAD];
} cap_t;

static void capture_cb(const buzz_frame_t *frame, void *ctx)
{
    cap_t *cap = ctx;
    cap->count++;
    cap->frame = *frame;
    memcpy(cap->payload, frame->payload, frame->length);
    cap->frame.payload = cap->payload;
}

static void test_tx_entries_decode(void)
{
    buzz_parser_t parser;
    cap_t cap = {0};
    buzz_parser_init(&parser, capture_cb, &cap);
    int tx_count = 0;
    for (size_t i = 0; i < kVectorsBasicSessionCount; i++) {
        const buzz_vector_entry_t *e = &kVectorsBasicSession[i];
        if (e->direction != BUZZ_VECTOR_DIR_TX) {
            continue;
        }
        tx_count++;
        int before = cap.count;
        CHECK_EQ_INT(buzz_parser_feed(&parser, e->frame, e->frame_len), BUZZ_OK);
        CHECK_EQ_INT(cap.count, before + 1); // 恰好交付一帧：无静默丢弃、无错帧
        CHECK_EQ_INT(cap.frame.version, BUZZ_VERSION_1);
        CHECK_EQ_INT(cap.frame.type, e->type);
        CHECK_EQ_INT(cap.frame.session, e->session);
        CHECK_EQ_INT(cap.frame.channel, e->channel);
        CHECK_EQ_INT(cap.frame.flags, 0);
        CHECK_EQ_INT(cap.frame.length, e->payload_len);
        CHECK(memcmp(cap.payload, e->payload, e->payload_len) == 0);
    }
    CHECK_EQ_INT(tx_count, 3); // PING / INFO_GET / LIST_REQ
}

static void test_rx_entries_encode_match(void)
{
    int rx_count = 0;
    for (size_t i = 0; i < kVectorsBasicSessionCount; i++) {
        const buzz_vector_entry_t *e = &kVectorsBasicSession[i];
        if (e->direction != BUZZ_VECTOR_DIR_RX) {
            continue;
        }
        rx_count++;
        uint8_t out[BUZZ_FRAME_HEADER_LEN + BUZZ_FRAME_MAX_PAYLOAD + BUZZ_FRAME_CRC_LEN];
        int n = buzz_frame_encode(out, sizeof(out), e->type, e->session, 0, e->channel,
                                  e->payload, (uint16_t)e->payload_len);
        CHECK_EQ_INT(n, (long)e->frame_len);
        if (n > 0) {
            CHECK(memcmp(out, e->frame, e->frame_len) == 0);
        }
    }
    CHECK_EQ_INT(rx_count, 3); // PONG / INFO / LIST_RESP
}

static void test_entry_count(void)
{
    CHECK_EQ_INT(kVectorsBasicSessionCount, 6);
}

int main(void)
{
    RUN_TEST(test_entry_count);
    RUN_TEST(test_tx_entries_decode);
    RUN_TEST(test_rx_entries_encode_match);
    return test_report();
}
