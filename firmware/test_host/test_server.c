// buzz_server 服务层单测（M4.2）：
//   - BUZZLINK §11 向量 V1~V4 经服务层管线（feed→PONG/分发/组帧发送）字节级比对
//   - 响应超 max_payload 自动分片（500B→3 片，逐片验证 FRAG/fragIndex/fragCount/
//     头字段一致）+ 分片大小随 set_max_payload 变化
//   - 分片请求重组后分发（2 片 LIST_REQ）、分片中插 PING 透传
//   - 未知 type/version/flags 保留位、坏帧 CRC → 无任何输出帧（§10）
//   - 控制/文件通道回调错误 → ERROR 帧（§9）；未注册回调 → 4999
//   - DL_ABORT→DL_END_RESP 映射、会话回显、主动发送（INFO session=0）
//   - 极简 JSON 助手（D2）、逐字节喂入、重组违规后流恢复
#include <stdint.h>
#include <string.h>

#include "buzz_server.h"
#include "crc16.h"
#include "test.h"

// ---------- §11 测试向量（与 test_frame.c 同源） ----------

static const uint8_t V1_PING[] = {0xbe, 0xec, 0x01, 0x01, 0x2a, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x86, 0x7d};
static const uint8_t V2_LIST_REQ[] = {0xbe, 0xec, 0x01, 0x10, 0x2b, 0x00, 0x01, 0x00,
                                      0x0c, 0x00, 0x7b, 0x22, 0x6f, 0x66, 0x66, 0x73,
                                      0x65, 0x74, 0x22, 0x3a, 0x30, 0x7d, 0xdf, 0xcd};
static const uint8_t V3_DL_BEGIN_REQ[] = {0xbe, 0xec, 0x01, 0x30, 0x31, 0x00, 0x02, 0x00,
                                          0x14, 0x00, 0x13, 0x52, 0x45, 0x43, 0x5f, 0x32,
                                          0x30, 0x32, 0x36, 0x31, 0x30, 0x31, 0x34, 0x5f,
                                          0x30, 0x39, 0x31, 0x35, 0x33, 0x33, 0xd5, 0x0c};
static const uint8_t V4_DL_BEGIN_RESP[] = {0xbe, 0xec, 0x01, 0x31, 0x31, 0x00, 0x02, 0x00,
                                           0x0d, 0x00, 0x00, 0xe0, 0x00, 0xa6, 0x11, 0x00,
                                           0x00, 0xac, 0x6e, 0x0f, 0x00, 0x3a, 0x6f, 0x02, 0x47};

// PING payload "abc"（sess 0x05）与其期望的 PONG 回显（CRC 独立计算硬编码）
static const uint8_t PING_ABC[] = {0xbe, 0xec, 0x01, 0x01, 0x05, 0x00, 0x00, 0x00,
                                   0x03, 0x00, 0x61, 0x62, 0x63, 0xcb, 0x67};
static const uint8_t PONG_ABC[] = {0xbe, 0xec, 0x01, 0x02, 0x05, 0x00, 0x00, 0x00,
                                   0x03, 0x00, 0x61, 0x62, 0x63, 0x04, 0xd6};
// V1 PING 的期望 PONG（空 payload 回显，sess 0x2A）
static const uint8_t PONG_V1[] = {0xbe, 0xec, 0x01, 0x02, 0x2a, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x04, 0xa5};

// ---------- mock 基础设施 ----------

// 发送捕获：帧按字节追加进缓冲，同时计数
typedef struct {
    uint8_t buf[2048];
    size_t len;
    int frames;
    int overflow;
} txlog_t;

static int mock_send(void *ctx, const uint8_t *frame, size_t len)
{
    txlog_t *t = ctx;
    t->frames++;
    if (len > sizeof(t->buf) - t->len) {
        t->overflow = 1;
        return -1;
    }
    memcpy(t->buf + t->len, frame, len);
    t->len += len;
    return 0;
}

// 业务回调 mock：记录入参，按配置返回响应或错误
#define MOCK_RESP_CAP 1024u
typedef struct {
    int calls;
    uint8_t req_type;
    uint8_t session;
    uint16_t channel;
    uint8_t req[128];
    uint32_t req_len;
    size_t resp_cap_seen;
    // 行为配置：ret ≥0 表示把 resp_body 前 resp_len 字节作为响应返回；
    // ret <0 走 ERROR 帧路径（err_code/err_msg）
    int ret;
    int err_code;
    const char *err_msg;
    uint8_t resp_body[MOCK_RESP_CAP];
    size_t resp_len;
} mock_t;

static int mock_handle(mock_t *m, const buzz_frame_t *req, uint8_t *resp, size_t resp_cap,
                       int *err_code, const char **err_msg)
{
    m->calls++;
    m->req_type = req->type;
    m->session = req->session;
    m->channel = req->channel;
    m->req_len = req->length;
    memcpy(m->req, req->payload, req->length);
    m->resp_cap_seen = resp_cap;
    if (m->ret < 0) {
        *err_code = m->err_code;
        *err_msg = m->err_msg;
        return -1;
    }
    memcpy(resp, m->resp_body, m->resp_len);
    return m->ret; // ret ≥0 时为响应字节数（resp_len 为实际拷贝长度，二者可不同以模拟契约违反）
}

static int ctl_handler(void *ctx, const buzz_frame_t *req, uint8_t *resp, size_t resp_cap,
                       int *err_code, const char **err_msg)
{
    return mock_handle(ctx, req, resp, resp_cap, err_code, err_msg);
}

static int file_handler(void *ctx, const buzz_frame_t *req, uint8_t *resp, size_t resp_cap,
                        int *err_code, const char **err_msg)
{
    return mock_handle(ctx, req, resp, resp_cap, err_code, err_msg);
}

// 实例静态分配（结构体内嵌 ~5KB 缓冲）；每用例 fixture 重置
static buzz_server_t srv;
static txlog_t txlog;
static mock_t ctl_mock;
static mock_t file_mock;

static void fixture(bool with_handlers)
{
    memset(&txlog, 0, sizeof(txlog));
    memset(&ctl_mock, 0, sizeof(ctl_mock));
    memset(&file_mock, 0, sizeof(file_mock));
    const buzz_server_io_t io = {
        .send = mock_send,
        .send_ctx = &txlog,
        .control = with_handlers ? ctl_handler : NULL,
        .control_ctx = &ctl_mock,
        .file = with_handlers ? file_handler : NULL,
        .file_ctx = &file_mock,
    };
    buzz_server_init(&srv, &io);
}

// 把捕获到的字节流重新解析成帧列表（用于逐帧校验分片字段）
typedef struct {
    int count;
    buzz_frame_t f[8];
    uint8_t pl[8][BUZZ_FRAME_MAX_PAYLOAD];
} fcap_t;

static void fcap_cb(const buzz_frame_t *frame, void *ctx)
{
    fcap_t *c = ctx;
    if (c->count >= 8) {
        return;
    }
    c->f[c->count] = *frame;
    memcpy(c->pl[c->count], frame->payload, frame->length);
    c->f[c->count].payload = c->pl[c->count];
    c->count++;
}

static void parse_txlog(fcap_t *c)
{
    buzz_parser_t p;
    memset(c, 0, sizeof(*c));
    buzz_parser_init(&p, fcap_cb, c);
    CHECK_EQ_INT(buzz_parser_feed(&p, txlog.buf, txlog.len), BUZZ_OK);
}

// 编码一帧（合法 type/flags）并喂入 server
static void feed_frame(uint8_t type, uint8_t session, uint16_t channel,
                       const uint8_t *payload, uint16_t len)
{
    uint8_t b[BUZZ_FRAME_HEADER_LEN + BUZZ_FRAME_MAX_PAYLOAD + BUZZ_FRAME_CRC_LEN];
    const int n = buzz_frame_encode(b, sizeof(b), type, session, 0, channel, payload, len);
    CHECK(n > 0);
    CHECK_EQ_INT(buzz_server_feed(&srv, b, (size_t)n), BUZZ_OK);
}

// 手工构造一帧（可指定 version/flags/type 任意值，CRC 按规范计算）
static size_t craft_frame(uint8_t *out, uint8_t version, uint8_t type, uint8_t session,
                          uint8_t flags, uint16_t channel, uint16_t len)
{
    out[0] = 0xbe;
    out[1] = 0xec;
    out[2] = version;
    out[3] = type;
    out[4] = session;
    out[5] = flags;
    out[6] = (uint8_t)(channel & 0xFFu);
    out[7] = (uint8_t)(channel >> 8);
    out[8] = (uint8_t)(len & 0xFFu);
    out[9] = (uint8_t)(len >> 8);
    for (uint16_t i = 0; i < len; i++) {
        out[10 + i] = (uint8_t)(i ^ 0xA5);
    }
    const uint16_t crc = buzz_crc16(out + 2, 8 + (size_t)len);
    out[10 + len] = (uint8_t)(crc & 0xFFu);
    out[10 + len + 1] = (uint8_t)(crc >> 8);
    return (size_t)(10 + len + 2);
}

// ---------- §11 向量 ----------

static void test_v1_ping_pong_exact_bytes(void)
{
    fixture(true);
    CHECK_EQ_INT(buzz_server_feed(&srv, V1_PING, sizeof(V1_PING)), BUZZ_OK);
    CHECK_EQ_INT(txlog.frames, 1);
    CHECK_EQ_INT(txlog.len, sizeof(PONG_V1));
    CHECK(memcmp(txlog.buf, PONG_V1, sizeof(PONG_V1)) == 0);
    CHECK_EQ_INT(ctl_mock.calls, 0);
    CHECK_EQ_INT(file_mock.calls, 0);
}

static void test_ping_payload_echoed(void)
{
    fixture(true);
    CHECK_EQ_INT(buzz_server_feed(&srv, PING_ABC, sizeof(PING_ABC)), BUZZ_OK);
    CHECK_EQ_INT(txlog.frames, 1);
    CHECK_EQ_INT(txlog.len, sizeof(PONG_ABC));
    CHECK(memcmp(txlog.buf, PONG_ABC, sizeof(PONG_ABC)) == 0);
}

static void test_v2_list_req_dispatched(void)
{
    fixture(true);
    static const char list_resp[] = "{\"status\":\"ok\",\"total\":0}";
    memcpy(ctl_mock.resp_body, list_resp, sizeof(list_resp) - 1);
    ctl_mock.resp_len = sizeof(list_resp) - 1;
    ctl_mock.ret = (int)ctl_mock.resp_len;

    CHECK_EQ_INT(buzz_server_feed(&srv, V2_LIST_REQ, sizeof(V2_LIST_REQ)), BUZZ_OK);
    CHECK_EQ_INT(ctl_mock.calls, 1);
    CHECK_EQ_INT(ctl_mock.req_type, BUZZ_TYPE_LIST_REQ);
    CHECK_EQ_INT(ctl_mock.session, 0x2b);
    CHECK_EQ_INT(ctl_mock.channel, BUZZ_CHANNEL_CONTROL);
    CHECK_EQ_INT(ctl_mock.req_len, 12);
    CHECK(memcmp(ctl_mock.req, "{\"offset\":0}", 12) == 0);
    CHECK_EQ_INT(ctl_mock.resp_cap_seen, BUZZ_SERVER_RESP_CAP);

    // 响应帧：type 映射 LIST_RESP、session 回显、payload 即回调产物
    fcap_t cap;
    parse_txlog(&cap);
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.f[0].type, BUZZ_TYPE_LIST_RESP);
    CHECK_EQ_INT(cap.f[0].session, 0x2b);
    CHECK_EQ_INT(cap.f[0].channel, BUZZ_CHANNEL_CONTROL);
    CHECK_EQ_INT(cap.f[0].flags, 0);
    CHECK_EQ_INT(cap.f[0].length, ctl_mock.resp_len);
    CHECK(memcmp(cap.f[0].payload, list_resp, ctl_mock.resp_len) == 0);

    // 狗喂 JSON 助手：从 mock 收到的请求里解析 offset
    long off = -1;
    CHECK(buzz_json_get_int((const char *)ctl_mock.req, "offset", &off) == BUZZ_OK);
    CHECK_EQ_INT(off, 0);
}

static void test_v3_dl_begin_dispatched_v4_response(void)
{
    fixture(true);
    // 回调返回 V4 的 13 字节响应体 → server 组帧应逐字节等于 V4 向量
    static const uint8_t body[] = {0x00, 0xe0, 0x00, 0xa6, 0x11, 0x00, 0x00, 0xac,
                                   0x6e, 0x0f, 0x00, 0x3a, 0x6f};
    memcpy(file_mock.resp_body, body, sizeof(body));
    file_mock.resp_len = sizeof(body);
    file_mock.ret = (int)file_mock.resp_len;

    CHECK_EQ_INT(buzz_server_feed(&srv, V3_DL_BEGIN_REQ, sizeof(V3_DL_BEGIN_REQ)), BUZZ_OK);
    CHECK_EQ_INT(file_mock.calls, 1);
    CHECK_EQ_INT(file_mock.req_type, BUZZ_TYPE_DL_BEGIN_REQ);
    CHECK_EQ_INT(file_mock.channel, BUZZ_CHANNEL_FILE);
    CHECK_EQ_INT(file_mock.req_len, 20);
    CHECK_EQ_INT(file_mock.req[0], 0x13);
    CHECK(memcmp(file_mock.req + 1, "REC_20261014_091533", 19) == 0);
    CHECK_EQ_INT(ctl_mock.calls, 0);

    CHECK_EQ_INT(txlog.frames, 1);
    CHECK_EQ_INT(txlog.len, sizeof(V4_DL_BEGIN_RESP));
    CHECK(memcmp(txlog.buf, V4_DL_BEGIN_RESP, sizeof(V4_DL_BEGIN_RESP)) == 0);
}

static void test_v4_proactive_send_exact_bytes(void)
{
    fixture(true);
    static const uint8_t body[] = {0x00, 0xe0, 0x00, 0xa6, 0x11, 0x00, 0x00, 0xac,
                                   0x6e, 0x0f, 0x00, 0x3a, 0x6f};
    CHECK_EQ_INT(buzz_server_send(&srv, BUZZ_TYPE_DL_BEGIN_RESP, 0x31, BUZZ_CHANNEL_FILE,
                                  body, sizeof(body)),
                 BUZZ_OK);
    CHECK_EQ_INT(txlog.frames, 1);
    CHECK_EQ_INT(txlog.len, sizeof(V4_DL_BEGIN_RESP));
    CHECK(memcmp(txlog.buf, V4_DL_BEGIN_RESP, sizeof(V4_DL_BEGIN_RESP)) == 0);
}

static void test_session_echoed(void)
{
    fixture(true);
    ctl_mock.resp_body[0] = '{';
    ctl_mock.resp_body[1] = '}';
    ctl_mock.resp_len = 2;
    ctl_mock.ret = 2;
    feed_frame(BUZZ_TYPE_INFO_GET, 0x55, BUZZ_CHANNEL_CONTROL, NULL, 0);
    CHECK_EQ_INT(ctl_mock.calls, 1);
    CHECK_EQ_INT(ctl_mock.session, 0x55);
    fcap_t cap;
    parse_txlog(&cap);
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.f[0].type, BUZZ_TYPE_INFO); // INFO_GET → INFO
    CHECK_EQ_INT(cap.f[0].session, 0x55);
}

// ---------- 响应分片 ----------

// 校验一条分片消息：cap 中 count 片，逐片 FRAG/idx/cnt/头字段一致，拼回后等于 ref
static void check_frag_stream(const fcap_t *cap, int count, uint8_t type, uint8_t session,
                              uint16_t channel, const uint8_t *ref, size_t ref_len,
                              const uint16_t *lens)
{
    CHECK_EQ_INT(cap->count, count);
    if (cap->count != count) {
        return;
    }
    uint8_t joined[512];
    size_t off = 0;
    for (int i = 0; i < count; i++) {
        const buzz_frame_t *f = &cap->f[i];
        CHECK_EQ_INT(f->version, BUZZ_VERSION_1);
        CHECK_EQ_INT(f->type, type);
        CHECK_EQ_INT(f->session, session);
        CHECK_EQ_INT(f->channel, channel);
        CHECK((f->flags & BUZZ_FLAG_FRAG) != 0u);
        uint16_t idx = 0, cnt = 0, dlen = 0;
        const uint8_t *data = NULL;
        CHECK(buzz_frag_parse(f, &idx, &cnt, &data, &dlen) == BUZZ_OK);
        CHECK_EQ_INT(idx, i);
        CHECK_EQ_INT(cnt, count);
        CHECK_EQ_INT(dlen, lens[i]);
        if (off + dlen <= sizeof(joined)) {
            memcpy(joined + off, data, dlen);
        }
        off += dlen;
    }
    CHECK_EQ_INT(off, ref_len);
    CHECK(memcmp(joined, ref, ref_len) == 0);
}

static void test_response_fragmentation_500b(void)
{
    fixture(true);
    static uint8_t big[500];
    for (uint32_t i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 7u + 3u);
    }
    memcpy(ctl_mock.resp_body, big, sizeof(big));
    ctl_mock.resp_len = sizeof(big);
    ctl_mock.ret = (int)ctl_mock.resp_len;

    CHECK_EQ_INT(buzz_server_feed(&srv, V2_LIST_REQ, sizeof(V2_LIST_REQ)), BUZZ_OK);
    CHECK_EQ_INT(ctl_mock.calls, 1);
    CHECK_EQ_INT(txlog.frames, 3); // 228 + 228 + 44（§11 分片示例）
    fcap_t cap;
    parse_txlog(&cap);
    static const uint16_t lens[3] = {228, 228, 44};
    check_frag_stream(&cap, 3, BUZZ_TYPE_LIST_RESP, 0x2b, BUZZ_CHANNEL_CONTROL, big,
                      sizeof(big), lens);
}

static void test_frag_size_follows_max_payload(void)
{
    fixture(true);
    static uint8_t big[500];
    for (uint32_t i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i ^ 0x5Au);
    }
    memcpy(ctl_mock.resp_body, big, sizeof(big));
    ctl_mock.resp_len = sizeof(big);
    ctl_mock.ret = (int)ctl_mock.resp_len;

    // 下限/上限校验
    CHECK_EQ_INT(buzz_server_set_max_payload(&srv, 4), BUZZ_ERR_ARG);
    CHECK_EQ_INT(buzz_server_set_max_payload(&srv, 233), BUZZ_ERR_ARG);
    CHECK_EQ_INT(buzz_server_set_max_payload(&srv, 0), BUZZ_ERR_ARG);
    CHECK_EQ_INT(buzz_server_set_max_payload(&srv, 100), BUZZ_OK);

    CHECK_EQ_INT(buzz_server_feed(&srv, V2_LIST_REQ, sizeof(V2_LIST_REQ)), BUZZ_OK);
    CHECK_EQ_INT(txlog.frames, 6); // 每片 data ≤ 96：96×5 + 20
    fcap_t cap;
    parse_txlog(&cap);
    static const uint16_t lens[6] = {96, 96, 96, 96, 96, 20};
    check_frag_stream(&cap, 6, BUZZ_TYPE_LIST_RESP, 0x2b, BUZZ_CHANNEL_CONTROL, big,
                      sizeof(big), lens);
}

// ---------- 分片请求 ----------

static void test_fragmented_request_reassembled(void)
{
    fixture(true);
    static const char j[] = "{\"offset\":7,\"limit\":3}"; // 23B
    uint8_t b0[64], b1[64];
    const int n0 = buzz_frag_encode(b0, sizeof(b0), BUZZ_TYPE_LIST_REQ, 0x2c,
                                    BUZZ_CHANNEL_CONTROL, 0, 2, (const uint8_t *)j, 12);
    const int n1 = buzz_frag_encode(b1, sizeof(b1), BUZZ_TYPE_LIST_REQ, 0x2c,
                                    BUZZ_CHANNEL_CONTROL, 1, 2, (const uint8_t *)j + 12, 11);
    CHECK(n0 > 0 && n1 > 0);
    CHECK_EQ_INT(buzz_server_feed(&srv, b0, (size_t)n0), BUZZ_OK);
    CHECK_EQ_INT(ctl_mock.calls, 0); // 未拼齐不分发
    CHECK_EQ_INT(buzz_server_feed(&srv, b1, (size_t)n1), BUZZ_OK);
    CHECK_EQ_INT(ctl_mock.calls, 1);
    CHECK_EQ_INT(ctl_mock.req_type, BUZZ_TYPE_LIST_REQ);
    CHECK_EQ_INT(ctl_mock.session, 0x2c);
    CHECK_EQ_INT(ctl_mock.req_len, 23);
    CHECK(memcmp(ctl_mock.req, j, 23) == 0);
    fcap_t cap;
    parse_txlog(&cap);
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.f[0].type, BUZZ_TYPE_LIST_RESP);
}

static void test_interleaved_ping_during_frag_request(void)
{
    fixture(true);
    static const char j[] = "{\"offset\":7,\"limit\":3}";
    uint8_t b0[64], b1[64];
    const int n0 = buzz_frag_encode(b0, sizeof(b0), BUZZ_TYPE_LIST_REQ, 0x2c,
                                    BUZZ_CHANNEL_CONTROL, 0, 2, (const uint8_t *)j, 12);
    const int n1 = buzz_frag_encode(b1, sizeof(b1), BUZZ_TYPE_LIST_REQ, 0x2c,
                                    BUZZ_CHANNEL_CONTROL, 1, 2, (const uint8_t *)j + 12, 11);
    CHECK_EQ_INT(buzz_server_feed(&srv, b0, (size_t)n0), BUZZ_OK);
    CHECK_EQ_INT(buzz_server_feed(&srv, V1_PING, sizeof(V1_PING)), BUZZ_OK); // 链路通道并行
    CHECK_EQ_INT(buzz_server_feed(&srv, b1, (size_t)n1), BUZZ_OK);

    fcap_t cap;
    parse_txlog(&cap);
    CHECK_EQ_INT(cap.count, 2);
    CHECK_EQ_INT(cap.f[0].type, BUZZ_TYPE_PONG); // PING 先回（重组不阻断链路通道）
    CHECK_EQ_INT(cap.f[0].session, 0x2a);
    CHECK_EQ_INT(cap.f[1].type, BUZZ_TYPE_LIST_RESP);
    CHECK_EQ_INT(cap.f[1].session, 0x2c);
    CHECK_EQ_INT(cap.f[1].length, 0); // mock 默认空响应
    CHECK_EQ_INT(ctl_mock.calls, 1);
}

// ---------- 静默忽略/容错（§10） ----------

static void feed_crafted_expect_silence(uint8_t version, uint8_t type, uint8_t flags,
                                        uint16_t channel, uint16_t len)
{
    fixture(true);
    uint8_t stream[64 + sizeof(V1_PING)];
    const size_t n = craft_frame(stream, version, type, 0x2a, flags, channel, len);
    memcpy(stream + n, V1_PING, sizeof(V1_PING)); // 后续合法帧不受影响
    CHECK_EQ_INT(buzz_server_feed(&srv, stream, n + sizeof(V1_PING)), BUZZ_OK);
    fcap_t cap;
    parse_txlog(&cap);
    CHECK_EQ_INT(cap.count, 1); // 仅 V1 的 PONG
    CHECK_EQ_INT(cap.f[0].type, BUZZ_TYPE_PONG);
    CHECK_EQ_INT(ctl_mock.calls, 0);
    CHECK_EQ_INT(file_mock.calls, 0);
}

static void test_unknown_type_no_output(void)
{
    feed_crafted_expect_silence(BUZZ_VERSION_1, 0x7E, 0x00, BUZZ_CHANNEL_CONTROL, 20);
}

static void test_unknown_version_no_output(void)
{
    feed_crafted_expect_silence(0x02, BUZZ_TYPE_INFO, 0x00, BUZZ_CHANNEL_CONTROL, 20);
}

static void test_reserved_flags_no_output(void)
{
    feed_crafted_expect_silence(BUZZ_VERSION_1, BUZZ_TYPE_INFO, 0x02, BUZZ_CHANNEL_CONTROL, 20);
}

static void test_bad_crc_no_output(void)
{
    fixture(true);
    uint8_t bad[sizeof(V1_PING)];
    memcpy(bad, V1_PING, sizeof(bad));
    bad[sizeof(bad) - 1] ^= 0xFF;
    CHECK_EQ_INT(buzz_server_feed(&srv, bad, sizeof(bad)), BUZZ_OK);
    CHECK_EQ_INT(txlog.frames, 0);
    CHECK_EQ_INT(ctl_mock.calls, 0);
    CHECK_EQ_INT(file_mock.calls, 0);
}

static void test_wrong_channel_known_type_ignored(void)
{
    fixture(true);
    // LIST_REQ 出现在文件通道 / DL_BEGIN_REQ 出现在控制通道：无分发规则，静默忽略
    feed_frame(BUZZ_TYPE_LIST_REQ, 0x2b, BUZZ_CHANNEL_FILE, (const uint8_t *)"{\"offset\":0}", 12);
    static const uint8_t dl_id[] = {0x01, 'X'};
    feed_frame(BUZZ_TYPE_DL_BEGIN_REQ, 0x31, BUZZ_CHANNEL_CONTROL, dl_id, sizeof(dl_id));
    CHECK_EQ_INT(txlog.frames, 0);
    CHECK_EQ_INT(ctl_mock.calls, 0);
    CHECK_EQ_INT(file_mock.calls, 0);
}

static void test_reasm_error_then_recovery(void)
{
    fixture(true);
    static const char j[] = "{\"offset\":7,\"limit\":3}";
    uint8_t b0[64], b1[64];
    const int n0 = buzz_frag_encode(b0, sizeof(b0), BUZZ_TYPE_LIST_REQ, 0x2c,
                                    BUZZ_CHANNEL_CONTROL, 0, 2, (const uint8_t *)j, 12);
    // 同 type/channel 但 session 不同的片：重组违规，整条消息作废
    const int n1 = buzz_frag_encode(b1, sizeof(b1), BUZZ_TYPE_LIST_REQ, 0x2d,
                                    BUZZ_CHANNEL_CONTROL, 1, 2, (const uint8_t *)j + 12, 11);
    CHECK_EQ_INT(buzz_server_feed(&srv, b0, (size_t)n0), BUZZ_OK);
    CHECK_EQ_INT(buzz_server_feed(&srv, b1, (size_t)n1), BUZZ_OK);
    CHECK_EQ_INT(txlog.frames, 0);
    CHECK_EQ_INT(ctl_mock.calls, 0);

    // 流保持可用：后续合法帧照常处理
    CHECK_EQ_INT(buzz_server_feed(&srv, V1_PING, sizeof(V1_PING)), BUZZ_OK);
    CHECK_EQ_INT(txlog.frames, 1);
    CHECK_EQ_INT(txlog.len, sizeof(PONG_V1));
    CHECK(memcmp(txlog.buf, PONG_V1, sizeof(PONG_V1)) == 0);
}

// ---------- ERROR 帧（§9） ----------

static void check_error_frame(const fcap_t *cap, uint8_t session, int code,
                              const char *msg)
{
    CHECK_EQ_INT(cap->count, 1);
    if (cap->count != 1) {
        return;
    }
    CHECK_EQ_INT(cap->f[0].type, BUZZ_TYPE_ERROR);
    CHECK_EQ_INT(cap->f[0].channel, BUZZ_CHANNEL_CONTROL); // ERROR 恒走控制通道
    CHECK_EQ_INT(cap->f[0].session, session);              // 原 session 回显
    char json[128];
    CHECK(cap->f[0].length < sizeof(json));
    memcpy(json, cap->f[0].payload, cap->f[0].length);
    json[cap->f[0].length] = '\0';
    long got_code = 0;
    CHECK(buzz_json_get_int(json, "code", &got_code) == BUZZ_OK);
    CHECK_EQ_INT(got_code, code);
    char got_msg[64];
    CHECK(buzz_json_get_str(json, "message", got_msg, sizeof(got_msg)) == BUZZ_OK);
    CHECK(strcmp(got_msg, msg) == 0);
}

static void test_control_handler_error_frame(void)
{
    fixture(true);
    ctl_mock.ret = -1;
    ctl_mock.err_code = BUZZ_ERR_CODE_NOT_FOUND;
    ctl_mock.err_msg = "file not found";
    CHECK_EQ_INT(buzz_server_feed(&srv, V2_LIST_REQ, sizeof(V2_LIST_REQ)), BUZZ_OK);
    fcap_t cap;
    parse_txlog(&cap);
    check_error_frame(&cap, 0x2b, 4004, "file not found");
}

static void test_file_handler_error_frame(void)
{
    fixture(true);
    file_mock.ret = -1;
    file_mock.err_code = BUZZ_ERR_CODE_BUSY;
    file_mock.err_msg = "busy";
    CHECK_EQ_INT(buzz_server_feed(&srv, V3_DL_BEGIN_REQ, sizeof(V3_DL_BEGIN_REQ)), BUZZ_OK);
    fcap_t cap;
    parse_txlog(&cap);
    check_error_frame(&cap, 0x31, 4009, "busy");
}

static void test_unregistered_control_handler_error_4999(void)
{
    fixture(false);
    CHECK_EQ_INT(buzz_server_feed(&srv, V2_LIST_REQ, sizeof(V2_LIST_REQ)), BUZZ_OK);
    fcap_t cap;
    parse_txlog(&cap);
    check_error_frame(&cap, 0x2b, 4999, "internal");
}

static void test_handler_over_cap_contract_violation(void)
{
    fixture(true);
    ctl_mock.ret = 5000; // 超过 resp_cap：契约违反 → 4999
    CHECK_EQ_INT(buzz_server_feed(&srv, V2_LIST_REQ, sizeof(V2_LIST_REQ)), BUZZ_OK);
    fcap_t cap;
    parse_txlog(&cap);
    check_error_frame(&cap, 0x2b, 4999, "internal");
}

// ---------- DL_ABORT 映射 / 主动发送 ----------

static void test_dl_abort_maps_to_dl_end_resp(void)
{
    fixture(true);
    static const uint8_t body[] = {BUZZ_STATUS_ABORTED, 0x12, 0x34};
    memcpy(file_mock.resp_body, body, sizeof(body));
    file_mock.resp_len = sizeof(body);
    file_mock.ret = (int)file_mock.resp_len;
    static const uint8_t abort_req[] = {0x01, 'X'};
    feed_frame(BUZZ_TYPE_DL_ABORT, 0x40, BUZZ_CHANNEL_FILE, abort_req, sizeof(abort_req));
    CHECK_EQ_INT(file_mock.calls, 1);
    CHECK_EQ_INT(file_mock.req_type, BUZZ_TYPE_DL_ABORT);
    fcap_t cap;
    parse_txlog(&cap);
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.f[0].type, BUZZ_TYPE_DL_END_RESP); // §5.1：ABORT 的响应是 END_RESP
    CHECK_EQ_INT(cap.f[0].session, 0x40);
    CHECK_EQ_INT(cap.f[0].channel, BUZZ_CHANNEL_FILE);
    CHECK_EQ_INT(cap.f[0].length, 3);
    CHECK(memcmp(cap.f[0].payload, body, 3) == 0);
}

static void test_proactive_info_session0(void)
{
    fixture(true);
    static const char info[] = "{\"model\":\"beecord-watch\"}";
    CHECK_EQ_INT(buzz_server_send(&srv, BUZZ_TYPE_INFO, 0x00, BUZZ_CHANNEL_CONTROL,
                                  (const uint8_t *)info, sizeof(info) - 1),
                 BUZZ_OK);
    fcap_t cap;
    parse_txlog(&cap);
    CHECK_EQ_INT(cap.count, 1);
    CHECK_EQ_INT(cap.f[0].type, BUZZ_TYPE_INFO);
    CHECK_EQ_INT(cap.f[0].session, 0x00);
    CHECK_EQ_INT(cap.f[0].channel, BUZZ_CHANNEL_CONTROL);
    CHECK_EQ_INT(cap.f[0].length, sizeof(info) - 1);
    CHECK(memcmp(cap.f[0].payload, info, sizeof(info) - 1) == 0);

    CHECK_EQ_INT(buzz_server_send(&srv, 0x7E, 0x00, BUZZ_CHANNEL_CONTROL, NULL, 0),
                 BUZZ_ERR_ARG); // 未知 type 拒绝
}

// ---------- 字节流任意切分 ----------

static void test_byte_by_byte_feed(void)
{
    fixture(true);
    for (size_t i = 0; i < sizeof(V1_PING); i++) {
        CHECK_EQ_INT(buzz_server_feed(&srv, V1_PING + i, 1), BUZZ_OK);
    }
    CHECK_EQ_INT(txlog.frames, 1);
    CHECK_EQ_INT(txlog.len, sizeof(PONG_V1));
    CHECK(memcmp(txlog.buf, PONG_V1, sizeof(PONG_V1)) == 0);
}

// ---------- JSON 助手（D2） ----------

static void test_json_get_int(void)
{
    long v = -1;
    CHECK(buzz_json_get_int("{\"offset\":0}", "offset", &v) == BUZZ_OK);
    CHECK_EQ_INT(v, 0);
    CHECK(buzz_json_get_int("{\"limit\":16,\"offset\":42}", "offset", &v) == BUZZ_OK);
    CHECK_EQ_INT(v, 42); // 字段顺序容忍
    CHECK(buzz_json_get_int("{ \"offset\" : 7 }", "offset", &v) == BUZZ_OK);
    CHECK_EQ_INT(v, 7); // 空白容忍
    CHECK(buzz_json_get_int("{\"offset\":-3}", "offset", &v) == BUZZ_OK);
    CHECK_EQ_INT(v, -3); // 负数
    // 未知字段（含嵌套 array/object）正确跳过，只匹配第 1 层键
    CHECK(buzz_json_get_int("{\"files\":[{\"id\":\"X\"}],\"offset\":9}", "offset", &v) ==
          BUZZ_OK);
    CHECK_EQ_INT(v, 9);

    CHECK(buzz_json_get_int("{\"limit\":16}", "offset", &v) == BUZZ_JSON_ERR_NOT_FOUND);
    CHECK(buzz_json_get_int("{\"offset\"}", "offset", &v) == BUZZ_JSON_ERR_MALFORMED);
    CHECK(buzz_json_get_int("{\"offset\":}", "offset", &v) == BUZZ_JSON_ERR_MALFORMED);
    CHECK(buzz_json_get_int("{\"offset\":\"abc\"}", "offset", &v) ==
          BUZZ_JSON_ERR_MALFORMED); // 类型不符
    CHECK(buzz_json_get_int("{\"offset\":3.14}", "offset", &v) ==
          BUZZ_JSON_ERR_MALFORMED); // 非整数
    CHECK(buzz_json_get_int("{\"offset\":12x}", "offset", &v) ==
          BUZZ_JSON_ERR_MALFORMED); // 非法数字
    CHECK(buzz_json_get_int(NULL, "offset", &v) == BUZZ_ERR_NULL);
}

static void test_json_get_str(void)
{
    char buf[64];
    CHECK(buzz_json_get_str("{\"id\":\"REC_1\"}", "id", buf, sizeof(buf)) == BUZZ_OK);
    CHECK(strcmp(buf, "REC_1") == 0);
    CHECK(buzz_json_get_str("{ \"id\" : \"REC_2\" }", "id", buf, sizeof(buf)) == BUZZ_OK);
    CHECK(strcmp(buf, "REC_2") == 0); // 空白容忍
    CHECK(buzz_json_get_str("{\"offset\":0,\"id\":\"REC_3\"}", "id", buf, sizeof(buf)) ==
          BUZZ_OK);
    CHECK(strcmp(buf, "REC_3") == 0); // 顺序容忍
    CHECK(buzz_json_get_str("{\"id\":\"A\\\"B\"}", "id", buf, sizeof(buf)) == BUZZ_OK);
    CHECK(strcmp(buf, "A\"B") == 0); // 转义容忍

    CHECK(buzz_json_get_str("{\"offset\":0}", "id", buf, sizeof(buf)) ==
          BUZZ_JSON_ERR_NOT_FOUND);
    CHECK(buzz_json_get_str("{\"id\":123}", "id", buf, sizeof(buf)) ==
          BUZZ_JSON_ERR_MALFORMED); // 值非字符串
    CHECK(buzz_json_get_str("{\"id\":\"REC_99\"}", "id", buf, 4) == BUZZ_ERR_SIZE); // cap 不足

    char small[4];
    CHECK(buzz_json_get_str("{\"id\":\"AB\"}", "id", small, sizeof(small)) == BUZZ_OK);
    CHECK(strcmp(small, "AB") == 0); // 边界：恰放得下（含 NUL）
}

int main(void)
{
    // §11 向量
    RUN_TEST(test_v1_ping_pong_exact_bytes);
    RUN_TEST(test_ping_payload_echoed);
    RUN_TEST(test_v2_list_req_dispatched);
    RUN_TEST(test_v3_dl_begin_dispatched_v4_response);
    RUN_TEST(test_v4_proactive_send_exact_bytes);
    RUN_TEST(test_session_echoed);
    // 响应分片
    RUN_TEST(test_response_fragmentation_500b);
    RUN_TEST(test_frag_size_follows_max_payload);
    // 分片请求
    RUN_TEST(test_fragmented_request_reassembled);
    RUN_TEST(test_interleaved_ping_during_frag_request);
    // 静默忽略/容错
    RUN_TEST(test_unknown_type_no_output);
    RUN_TEST(test_unknown_version_no_output);
    RUN_TEST(test_reserved_flags_no_output);
    RUN_TEST(test_bad_crc_no_output);
    RUN_TEST(test_wrong_channel_known_type_ignored);
    RUN_TEST(test_reasm_error_then_recovery);
    // ERROR 帧
    RUN_TEST(test_control_handler_error_frame);
    RUN_TEST(test_file_handler_error_frame);
    RUN_TEST(test_unregistered_control_handler_error_4999);
    RUN_TEST(test_handler_over_cap_contract_violation);
    // 映射/主动发送
    RUN_TEST(test_dl_abort_maps_to_dl_end_resp);
    RUN_TEST(test_proactive_info_session0);
    // 字节流切分 + JSON 助手
    RUN_TEST(test_byte_by_byte_feed);
    RUN_TEST(test_json_get_int);
    RUN_TEST(test_json_get_str);
    return test_report();
}
