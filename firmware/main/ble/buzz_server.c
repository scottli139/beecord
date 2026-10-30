// BuzzLink 设备端协议服务层（M4.2）实现：分发/组帧/分片/ERROR 帧 + 极简 JSON 助手。
// 硬件无关：仅依赖 libc 与协议层（buzz_proto/buzz_reasm/crc16），可宿主单测。
#include "buzz_server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// (channel, 请求 type) → 响应 type（§5.1；DL_ABORT 的响应是 DL_END_RESP）。
// 通道与 type 不匹配的组合（如 LIST_REQ 出现在文件通道）返回 0，
// 与非请求帧（PONG/INFO/DL_CHUNK 等 D→C 帧）一样静默忽略。
static uint8_t response_type(uint16_t channel, uint8_t req_type)
{
    if (channel == BUZZ_CHANNEL_CONTROL) {
        switch (req_type) {
        case BUZZ_TYPE_INFO_GET:
            return BUZZ_TYPE_INFO;
        case BUZZ_TYPE_LIST_REQ:
            return BUZZ_TYPE_LIST_RESP;
        case BUZZ_TYPE_FILE_INFO_REQ:
            return BUZZ_TYPE_FILE_INFO_RESP;
        case BUZZ_TYPE_FILE_DELETE_REQ:
            return BUZZ_TYPE_FILE_DELETE_RESP;
        case BUZZ_TYPE_TIME_SET_REQ:
            return BUZZ_TYPE_TIME_SET_RESP;
        default:
            return 0;
        }
    }
    if (channel == BUZZ_CHANNEL_FILE) {
        switch (req_type) {
        case BUZZ_TYPE_DL_BEGIN_REQ:
            return BUZZ_TYPE_DL_BEGIN_RESP;
        case BUZZ_TYPE_DL_END_REQ:
            return BUZZ_TYPE_DL_END_RESP;
        case BUZZ_TYPE_DL_ABORT:
            return BUZZ_TYPE_DL_END_RESP;
        default:
            return 0;
        }
    }
    return 0;
}

// 组帧发送：len ≤ max_payload 单帧；否则按 §3.3 分片（FRAG + fragIndex/fragCount，
// 每片 data ≤ max_payload-4）。逐帧调发送回调，任一次失败即中止。
// payload 为调用方缓冲，本层只读（可为 NULL 当且仅当 len == 0）。
static int emit(buzz_server_t *s, uint8_t type, uint8_t session, uint16_t channel,
                const uint8_t *payload, size_t len)
{
    if (s->io.send == NULL) {
        return BUZZ_SERVER_ERR_SEND;
    }
    if (len <= (size_t)s->max_payload) {
        int n = buzz_frame_encode(s->tx, sizeof(s->tx), type, session, 0, channel, payload,
                                  (uint16_t)len);
        if (n < 0) {
            return n;
        }
        return s->io.send(s->io.send_ctx, s->tx, (size_t)n) < 0 ? BUZZ_SERVER_ERR_SEND : BUZZ_OK;
    }

    const size_t cap = (size_t)s->max_payload - BUZZ_FRAG_HEADER_LEN; // 每片数据上限（≥1）
    const size_t count = (len + cap - 1u) / cap;
    if (count > 0xFFFFu) {
        return BUZZ_ERR_SIZE; // 分片数超 u16（仅 max_payload 极小且 len 近 64 KB 时可达）
    }
    for (uint16_t i = 0; i < (uint16_t)count; i++) {
        const size_t off = (size_t)i * cap;
        const uint16_t dlen = (uint16_t)((len - off > cap) ? cap : len - off);
        int n = buzz_frag_encode(s->tx, sizeof(s->tx), type, session, channel, i,
                                 (uint16_t)count, payload + off, dlen);
        if (n < 0) {
            return n;
        }
        if (s->io.send(s->io.send_ctx, s->tx, (size_t)n) < 0) {
            return BUZZ_SERVER_ERR_SEND;
        }
    }
    return BUZZ_OK;
}

// 控制通道错误统一出口：ERROR 帧（type 0x7F、控制通道、session 回显，§9）。
static void send_error(buzz_server_t *s, uint8_t session, int code, const char *msg)
{
    int n = snprintf((char *)s->resp, BUZZ_SERVER_RESP_CAP, "{\"code\":%d,\"message\":\"%s\"}",
                     code, msg);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= BUZZ_SERVER_RESP_CAP) {
        n = (int)BUZZ_SERVER_RESP_CAP - 1; // 截断防御：msg 为短常量，实际不可达
    }
    (void)emit(s, BUZZ_TYPE_ERROR, session, BUZZ_CHANNEL_CONTROL, s->resp, (size_t)n);
}

// 分发一条完整消息：PING 内置 PONG；控制/文件通道按映射表找回调；
// 其余（非请求 type、已知 type 但通道不符、无映射）静默忽略（§10）。
static void dispatch(buzz_server_t *s, const buzz_frame_t *req)
{
    if (req->type == BUZZ_TYPE_PING && req->channel == BUZZ_CHANNEL_LINK) {
        (void)emit(s, BUZZ_TYPE_PONG, req->session, BUZZ_CHANNEL_LINK, req->payload,
                   req->length); // payload 原样回显（通常为空）
        return;
    }

    const uint8_t rtype = response_type(req->channel, req->type);
    if (rtype == 0u) {
        return;
    }
    buzz_cmd_handler_t handler = NULL;
    void *hctx = NULL;
    if (req->channel == BUZZ_CHANNEL_CONTROL) {
        handler = s->io.control;
        hctx = s->io.control_ctx;
    } else {
        handler = s->io.file; // 走到这里必为文件通道（其余通道已在上面返回）
        hctx = s->io.file_ctx;
    }
    if (handler == NULL) {
        send_error(s, req->session, BUZZ_ERR_CODE_INTERNAL, "internal");
        return;
    }

    int err_code = 0;
    const char *err_msg = NULL;
    const int n = handler(hctx, req, s->resp, BUZZ_SERVER_RESP_CAP, &err_code, &err_msg);
    if (n < 0) {
        send_error(s, req->session, err_code != 0 ? err_code : BUZZ_ERR_CODE_INTERNAL,
                   err_msg != NULL ? err_msg : "internal");
        return;
    }
    if ((size_t)n > BUZZ_SERVER_RESP_CAP) {
        send_error(s, req->session, BUZZ_ERR_CODE_INTERNAL, "internal"); // 回调契约违反
        return;
    }
    (void)emit(s, rtype, req->session, req->channel, s->resp, (size_t)n);
}

// 解析器交付一帧：先过分片重组，拼齐/透传成完整消息再分发。
static void frame_cb(const buzz_frame_t *frame, void *ctx)
{
    buzz_server_t *s = ctx;
    const buzz_frame_t *msg = NULL;
    const int rc = buzz_reasm_feed(&s->reasm, frame, &msg);
    if (rc != BUZZ_OK || msg == NULL) {
        return; // 分片未齐 / 重组违规：静默丢弃（§10）
    }
    dispatch(s, msg);
}

void buzz_server_init(buzz_server_t *s, const buzz_server_io_t *io)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    if (io != NULL) {
        s->io = *io;
    }
    s->max_payload = BUZZ_FRAME_MAX_PAYLOAD;
    buzz_parser_init(&s->parser, frame_cb, s);
    buzz_reasm_init(&s->reasm, s->req_buf, BUZZ_SERVER_REQ_CAP, s->frag_bitmap,
                    BUZZ_SERVER_FRAG_SLOTS);
}

int buzz_server_feed(buzz_server_t *s, const uint8_t *data, size_t len)
{
    if (s == NULL || (data == NULL && len > 0)) {
        return BUZZ_ERR_NULL;
    }
    return buzz_parser_feed(&s->parser, data, len);
}

int buzz_server_send(buzz_server_t *s, uint8_t type, uint8_t session, uint16_t channel,
                     const uint8_t *payload, size_t len)
{
    if (s == NULL || (payload == NULL && len > 0)) {
        return BUZZ_ERR_NULL;
    }
    if (!buzz_type_is_known(type)) {
        return BUZZ_ERR_ARG;
    }
    if (len > BUZZ_MSG_MAX_LEN) {
        return BUZZ_ERR_SIZE;
    }
    return emit(s, type, session, channel, payload, len);
}

int buzz_server_set_max_payload(buzz_server_t *s, uint16_t n)
{
    if (s == NULL) {
        return BUZZ_ERR_NULL;
    }
    if (n < BUZZ_FRAG_HEADER_LEN + 1u || n > BUZZ_FRAME_MAX_PAYLOAD) {
        return BUZZ_ERR_ARG;
    }
    s->max_payload = n;
    return BUZZ_OK;
}

// ---------- 极简 JSON 助手（D2） ----------

// 跳过 JSON 空白。
static const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

// 跳过一个值（string/number/true/false/null/array/object），返回其后的分隔符位置。
// 非法结构返回 NULL。string 内容忍 \\ 转义；嵌套 array/object 按深度配对。
static const char *json_skip_value(const char *p)
{
    if (*p == '"') {
        p++;
        while (*p != '\0' && *p != '"') {
            if (*p == '\\' && p[1] != '\0') {
                p++;
            }
            p++;
        }
        return (*p == '"') ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (;;) {
            const char c = *p;
            if (c == '\0') {
                return NULL;
            }
            if (c == '"') { // 嵌套字符串整体跳过（其中的括号不计深度）
                p++;
                while (*p != '\0' && *p != '"') {
                    if (*p == '\\' && p[1] != '\0') {
                        p++;
                    }
                    p++;
                }
                if (*p != '"') {
                    return NULL;
                }
                p++;
                continue;
            }
            if (c == '{' || c == '[') {
                depth++;
            } else if (c == '}' || c == ']') {
                depth--;
                if (depth == 0) {
                    return p + 1;
                }
            }
            p++;
        }
    }
    // number/true/false/null：读到值后分隔符（空白/逗号/右括号/结束）
    while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' &&
           *p != '\r' && *p != '\n') {
        p++;
    }
    return p;
}

// 在扁平对象中查找 key 的值起点（跳过的空白已去除）。
// 找到返回指向值首字符的指针；未找到返回 NULL 且 *malformed=false；
// 结构非法返回 NULL 且 *malformed=true。
static const char *json_find_value(const char *json, const char *key, bool *malformed)
{
    const size_t klen = strlen(key);
    const char *p = json_skip_ws(json);
    if (*p != '{') {
        *malformed = true;
        return NULL;
    }
    p++;
    for (;;) {
        p = json_skip_ws(p);
        if (*p == '}') {
            *malformed = false; // 正常走到对象尾：键不存在
            return NULL;
        }
        if (*p != '"') {
            *malformed = true;
            return NULL;
        }
        p++;
        const bool match = strncmp(p, key, klen) == 0 && p[klen] == '"';
        while (*p != '\0' && *p != '"') { // 扫描到键结束引号（容忍转义）
            if (*p == '\\' && p[1] != '\0') {
                p++;
            }
            p++;
        }
        if (*p != '"') {
            *malformed = true;
            return NULL;
        }
        p++; // 越过键结束引号
        p = json_skip_ws(p);
        if (*p != ':') {
            *malformed = true;
            return NULL;
        }
        p++;
        p = json_skip_ws(p);
        if (match) {
            *malformed = false;
            return p;
        }
        p = json_skip_value(p);
        if (p == NULL) {
            *malformed = true;
            return NULL;
        }
        p = json_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            *malformed = false;
            return NULL;
        }
        *malformed = true;
        return NULL;
    }
}

int buzz_json_get_int(const char *json, const char *key, long *out)
{
    if (json == NULL || key == NULL || out == NULL) {
        return BUZZ_ERR_NULL;
    }
    bool malformed = false;
    const char *v = json_find_value(json, key, &malformed);
    if (v == NULL) {
        return malformed ? BUZZ_JSON_ERR_MALFORMED : BUZZ_JSON_ERR_NOT_FOUND;
    }
    if (*v == '"' || *v == '{' || *v == '[') {
        return BUZZ_JSON_ERR_MALFORMED; // 值类型不符
    }
    errno = 0;
    char *end = NULL;
    const long val = strtol(v, &end, 10);
    if (end == v || errno == ERANGE) {
        return BUZZ_JSON_ERR_MALFORMED;
    }
    const char *d = json_skip_ws(end);
    if (*d != ',' && *d != '}') {
        return BUZZ_JSON_ERR_MALFORMED; // 数字后必须跟合法分隔（拒 3.14/12x）
    }
    *out = val;
    return BUZZ_OK;
}

int buzz_json_get_i64(const char *json, const char *key, int64_t *out)
{
    if (json == NULL || key == NULL || out == NULL) {
        return BUZZ_ERR_NULL;
    }
    bool malformed = false;
    const char *v = json_find_value(json, key, &malformed);
    if (v == NULL) {
        return malformed ? BUZZ_JSON_ERR_MALFORMED : BUZZ_JSON_ERR_NOT_FOUND;
    }
    if (*v == '"' || *v == '{' || *v == '[') {
        return BUZZ_JSON_ERR_MALFORMED; // 值类型不符
    }
    errno = 0;
    char *end = NULL;
    const int64_t val = strtoll(v, &end, 10);
    if (end == v || errno == ERANGE) {
        return BUZZ_JSON_ERR_MALFORMED;
    }
    const char *d = json_skip_ws(end);
    if (*d != ',' && *d != '}') {
        return BUZZ_JSON_ERR_MALFORMED;
    }
    *out = val;
    return BUZZ_OK;
}

int buzz_json_get_str(const char *json, const char *key, char *buf, size_t cap)
{
    if (json == NULL || key == NULL || (buf == NULL && cap > 0)) {
        return BUZZ_ERR_NULL;
    }
    bool malformed = false;
    const char *v = json_find_value(json, key, &malformed);
    if (v == NULL) {
        return malformed ? BUZZ_JSON_ERR_MALFORMED : BUZZ_JSON_ERR_NOT_FOUND;
    }
    if (*v != '"') {
        return BUZZ_JSON_ERR_MALFORMED; // 值非字符串
    }
    v++;
    size_t n = 0;
    while (*v != '\0' && *v != '"') {
        char c = *v;
        if (c == '\\') { // 极简转义：反斜杠后一字按原样拷贝
            v++;
            c = *v;
            if (c == '\0') {
                return BUZZ_JSON_ERR_MALFORMED;
            }
        }
        if (n + 1u >= cap) {
            return BUZZ_ERR_SIZE;
        }
        buf[n++] = c;
        v++;
    }
    if (*v != '"') {
        return BUZZ_JSON_ERR_MALFORMED; // 字符串未终止
    }
    if (cap > 0) {
        buf[n] = '\0';
    }
    return BUZZ_OK;
}
