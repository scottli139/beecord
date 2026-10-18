// 帧编解码 + 字节流解析单测：BUZZLINK §11 向量 V1~V4 字段断言与编码往返，
// 以及重同步/容错路径（CRC 错帧、垃圾前缀、未知 version/type/flags、超长度）。
#include <stdint.h>
#include <string.h>

#include "buzz_proto.h"
#include "crc16.h"
#include "test.h"

// §11 测试向量（含 CRC，可直接用于解码断言与编码往返）
static const uint8_t V1[] = {0xbe, 0xec, 0x01, 0x01, 0x2a, 0x00, 0x00, 0x00,
                             0x00, 0x00, 0x86, 0x7d};
static const uint8_t V2[] = {0xbe, 0xec, 0x01, 0x10, 0x2b, 0x00, 0x01, 0x00,
                             0x0c, 0x00, 0x7b, 0x22, 0x6f, 0x66, 0x66, 0x73,
                             0x65, 0x74, 0x22, 0x3a, 0x30, 0x7d, 0xdf, 0xcd};
static const uint8_t V3[] = {0xbe, 0xec, 0x01, 0x30, 0x31, 0x00, 0x02, 0x00,
                             0x14, 0x00, 0x13, 0x52, 0x45, 0x43, 0x5f, 0x32,
                             0x30, 0x32, 0x36, 0x31, 0x30, 0x31, 0x34, 0x5f,
                             0x30, 0x39, 0x31, 0x35, 0x33, 0x33, 0xd5, 0x0c};
static const uint8_t V4[] = {0xbe, 0xec, 0x01, 0x31, 0x31, 0x00, 0x02, 0x00,
                             0x0d, 0x00, 0x00, 0xe0, 0x00, 0xa6, 0x11, 0x00,
                             0x00, 0xac, 0x6e, 0x0f, 0x00, 0x3a, 0x6f, 0x02, 0x47};

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

static void feed_all(buzz_parser_t *parser, const uint8_t *data, size_t len)
{
    CHECK_EQ_INT(buzz_parser_feed(parser, data, len), BUZZ_OK);
}

// 用向量里的字段重新编码，要求逐字节一致（CRC 正确性内建于往返中）
static void check_encode_roundtrip(const uint8_t *vec, size_t vec_len)
{
    buzz_frame_t f = {
        .version = vec[2], .type = vec[3], .session = vec[4], .flags = vec[5],
        .channel = (uint16_t)(vec[6] | ((uint16_t)vec[7] << 8)),
        .length = (uint16_t)(vec[8] | ((uint16_t)vec[9] << 8)),
        .payload = vec + 10,
    };
    uint8_t out[BUZZ_FRAME_HEADER_LEN + BUZZ_FRAME_MAX_PAYLOAD + BUZZ_FRAME_CRC_LEN];
    int n = buzz_frame_encode(out, sizeof(out), f.type, f.session, f.flags, f.channel,
                              f.payload, (uint16_t)f.length);
    CHECK_EQ_INT(n, (long)vec_len);
    CHECK(memcmp(out, vec, vec_len) == 0);
}

static void test_decode_v1_ping(void)
{
    buzz_parser_t p;
    cap_t cap = {0};
    buzz_parser_init(&p, capture_cb, &cap);
    feed_all(&p, V1, sizeof(V1));
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.frame.version, BUZZ_VERSION_1);
    CHECK_EQ_INT(cap.frame.type, BUZZ_TYPE_PING);
    CHECK_EQ_INT(cap.frame.session, 0x2a);
    CHECK_EQ_INT(cap.frame.flags, 0);
    CHECK_EQ_INT(cap.frame.channel, BUZZ_CHANNEL_LINK);
    CHECK_EQ_INT(cap.frame.length, 0);
}

static void test_decode_v2_list_req(void)
{
    buzz_parser_t p;
    cap_t cap = {0};
    buzz_parser_init(&p, capture_cb, &cap);
    feed_all(&p, V2, sizeof(V2));
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.frame.type, BUZZ_TYPE_LIST_REQ);
    CHECK_EQ_INT(cap.frame.session, 0x2b);
    CHECK_EQ_INT(cap.frame.channel, BUZZ_CHANNEL_CONTROL);
    CHECK_EQ_INT(cap.frame.length, 12);
    static const uint8_t json[] = "{\"offset\":0}";
    CHECK(memcmp(cap.payload, json, 12) == 0);
}

static void test_decode_v3_dl_begin_req(void)
{
    buzz_parser_t p;
    cap_t cap = {0};
    buzz_parser_init(&p, capture_cb, &cap);
    feed_all(&p, V3, sizeof(V3));
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.frame.type, BUZZ_TYPE_DL_BEGIN_REQ);
    CHECK_EQ_INT(cap.frame.channel, BUZZ_CHANNEL_FILE);
    CHECK_EQ_INT(cap.frame.length, 20);
    CHECK_EQ_INT(cap.payload[0], 0x13); // idLen = 19
    CHECK(memcmp(cap.payload + 1, "REC_20261014_091533", 19) == 0);
}

static void test_decode_v4_dl_begin_resp(void)
{
    buzz_parser_t p;
    cap_t cap = {0};
    buzz_parser_init(&p, capture_cb, &cap);
    feed_all(&p, V4, sizeof(V4));
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.frame.type, BUZZ_TYPE_DL_BEGIN_RESP);
    static const uint8_t body[] = {0x00, 0xe0, 0x00, 0xa6, 0x11, 0x00, 0x00, 0xac,
                                   0x6e, 0x0f, 0x00, 0x3a, 0x6f};
    CHECK_EQ_INT(cap.frame.length, 13);
    CHECK(memcmp(cap.payload, body, 13) == 0); // ok/chunkDataLen=224/total=4518/size/crc
}

static void test_encode_roundtrip_all_vectors(void)
{
    check_encode_roundtrip(V1, sizeof(V1));
    check_encode_roundtrip(V2, sizeof(V2));
    check_encode_roundtrip(V3, sizeof(V3));
    check_encode_roundtrip(V4, sizeof(V4));
}

static void test_crc_corrupt_frame_rejected(void)
{
    uint8_t bad[sizeof(V1)];
    memcpy(bad, V1, sizeof(bad));
    bad[sizeof(bad) - 1] ^= 0xFF; // 破坏 CRC
    buzz_parser_t p;
    cap_t cap = {0};
    buzz_parser_init(&p, capture_cb, &cap);
    feed_all(&p, bad, sizeof(bad));
    CHECK_EQ_INT(cap.count, 0); // 弃帧
}

static void test_byte_by_byte_feed(void)
{
    buzz_parser_t p;
    cap_t cap = {0};
    buzz_parser_init(&p, capture_cb, &cap);
    for (size_t i = 0; i < sizeof(V3); i++) {
        feed_all(&p, V3 + i, 1);
    }
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.frame.type, BUZZ_TYPE_DL_BEGIN_REQ);
    CHECK_EQ_INT(cap.frame.length, 20);
}

static void test_two_frames_single_feed(void)
{
    uint8_t stream[sizeof(V1) + sizeof(V2)];
    memcpy(stream, V1, sizeof(V1));
    memcpy(stream + sizeof(V1), V2, sizeof(V2));
    buzz_parser_t p;
    cap_t cap = {0};
    buzz_parser_init(&p, capture_cb, &cap);
    feed_all(&p, stream, sizeof(stream));
    CHECK_EQ_INT(cap.count, 2);
    CHECK_EQ_INT(cap.frame.type, BUZZ_TYPE_LIST_REQ); // 最后一帧
}

static void test_garbage_prefix_resync(void)
{
    uint8_t stream[4 + sizeof(V1)] = {0x00, 0xbe, 0x11, 0x22}; // 含假魔数 be 11
    memcpy(stream + 4, V1, sizeof(V1));
    buzz_parser_t p;
    cap_t cap = {0};
    buzz_parser_init(&p, capture_cb, &cap);
    feed_all(&p, stream, sizeof(stream));
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.frame.type, BUZZ_TYPE_PING);
}

static void test_double_magic_resync(void)
{
    uint8_t stream[2 + sizeof(V1)] = {0xbe, 0xbe}; // 首个 be 是假魔数，第二个开启真帧
    memcpy(stream + 2, V1, sizeof(V1));
    buzz_parser_t p;
    cap_t cap = {0};
    buzz_parser_init(&p, capture_cb, &cap);
    feed_all(&p, stream, sizeof(stream));
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.frame.type, BUZZ_TYPE_PING);
}

// 构造一帧：type/version/flags 自定义，payload 填充 pattern，CRC 始终按规范计算
static size_t craft_frame(uint8_t *out, uint8_t version, uint8_t type, uint8_t flags,
                          uint16_t len)
{
    out[0] = 0xbe;
    out[1] = 0xec;
    out[2] = version;
    out[3] = type;
    out[4] = 0x2a;
    out[5] = flags;
    out[6] = 0x01;
    out[7] = 0x00;
    out[8] = (uint8_t)(len & 0xFF);
    out[9] = (uint8_t)(len >> 8);
    for (uint16_t i = 0; i < len; i++) {
        out[10 + i] = (uint8_t)(i ^ 0xA5);
    }
    uint16_t crc = buzz_crc16(out + 2, 8 + (size_t)len);
    out[10 + len] = (uint8_t)(crc & 0xFF);
    out[10 + len + 1] = (uint8_t)(crc >> 8);
    return (size_t)(10 + len + 2);
}

static void test_unknown_type_ignored(void)
{
    uint8_t stream[64 + sizeof(V1)];
    size_t n = craft_frame(stream, BUZZ_VERSION_1, 0x7E, 0x00, 20); // 0x7E 未定义
    memcpy(stream + n, V1, sizeof(V1));
    buzz_parser_t p;
    cap_t cap = {0};
    buzz_parser_init(&p, capture_cb, &cap);
    feed_all(&p, stream, n + sizeof(V1));
    CHECK_EQ_INT(cap.count, 1); // 仅 V1 交付
    CHECK_EQ_INT(cap.frame.type, BUZZ_TYPE_PING);
}

static void test_unknown_version_ignored(void)
{
    uint8_t stream[64 + sizeof(V1)];
    size_t n = craft_frame(stream, 0x02, BUZZ_TYPE_INFO, 0x00, 20); // 未来版本
    memcpy(stream + n, V1, sizeof(V1));
    buzz_parser_t p;
    cap_t cap = {0};
    buzz_parser_init(&p, capture_cb, &cap);
    feed_all(&p, stream, n + sizeof(V1));
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.frame.type, BUZZ_TYPE_PING);
}

static void test_reserved_flags_bit_ignored(void)
{
    uint8_t stream[64 + sizeof(V1)];
    size_t n = craft_frame(stream, BUZZ_VERSION_1, BUZZ_TYPE_INFO, 0x02, 20); // bit1 保留
    memcpy(stream + n, V1, sizeof(V1));
    buzz_parser_t p;
    cap_t cap = {0};
    buzz_parser_init(&p, capture_cb, &cap);
    feed_all(&p, stream, n + sizeof(V1));
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.frame.type, BUZZ_TYPE_PING);
}

static void test_oversized_payload_ignored(void)
{
    // length=233 超 v1 单帧上限：整帧被跳过（含有效 CRC），后续帧不受影响
    uint8_t stream[300 + sizeof(V1)];
    size_t n = craft_frame(stream, BUZZ_VERSION_1, BUZZ_TYPE_INFO, 0x00, 233);
    memcpy(stream + n, V1, sizeof(V1));
    buzz_parser_t p;
    cap_t cap = {0};
    buzz_parser_init(&p, capture_cb, &cap);
    feed_all(&p, stream, n + sizeof(V1));
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.frame.type, BUZZ_TYPE_PING);
}

int main(void)
{
    RUN_TEST(test_decode_v1_ping);
    RUN_TEST(test_decode_v2_list_req);
    RUN_TEST(test_decode_v3_dl_begin_req);
    RUN_TEST(test_decode_v4_dl_begin_resp);
    RUN_TEST(test_encode_roundtrip_all_vectors);
    RUN_TEST(test_crc_corrupt_frame_rejected);
    RUN_TEST(test_byte_by_byte_feed);
    RUN_TEST(test_two_frames_single_feed);
    RUN_TEST(test_garbage_prefix_resync);
    RUN_TEST(test_double_magic_resync);
    RUN_TEST(test_unknown_type_ignored);
    RUN_TEST(test_unknown_version_ignored);
    RUN_TEST(test_reserved_flags_bit_ignored);
    RUN_TEST(test_oversized_payload_ignored);
    return test_report();
}
