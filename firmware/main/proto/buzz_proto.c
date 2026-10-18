// BuzzLink 协议层 —— 帧编码与字节流解析实现。硬件无关，可宿主单测。
#include "buzz_proto.h"

#include <string.h>

#include "crc16.h"

// 小端读写辅助
static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

bool buzz_type_is_known(uint8_t type)
{
    switch (type) {
    case BUZZ_TYPE_PING:
    case BUZZ_TYPE_PONG:
    case BUZZ_TYPE_INFO:
    case BUZZ_TYPE_INFO_GET:
    case BUZZ_TYPE_LIST_REQ:
    case BUZZ_TYPE_LIST_RESP:
    case BUZZ_TYPE_FILE_INFO_REQ:
    case BUZZ_TYPE_FILE_INFO_RESP:
    case BUZZ_TYPE_DL_BEGIN_REQ:
    case BUZZ_TYPE_DL_BEGIN_RESP:
    case BUZZ_TYPE_DL_CHUNK:
    case BUZZ_TYPE_DL_END_REQ:
    case BUZZ_TYPE_DL_END_RESP:
    case BUZZ_TYPE_DL_ABORT:
    case BUZZ_TYPE_FILE_DELETE_REQ:
    case BUZZ_TYPE_FILE_DELETE_RESP:
    case BUZZ_TYPE_ERROR:
        return true;
    default:
        return false;
    }
}

int buzz_frame_encode(uint8_t *out, size_t out_cap, uint8_t type, uint8_t session,
                      uint8_t flags, uint16_t channel, const uint8_t *payload,
                      uint16_t payload_len)
{
    if (out == NULL) {
        return BUZZ_ERR_NULL;
    }
    if (payload == NULL && payload_len > 0) {
        return BUZZ_ERR_NULL;
    }
    if (!buzz_type_is_known(type) || (flags & (uint8_t)~BUZZ_FLAG_FRAG) != 0u) {
        return BUZZ_ERR_ARG;
    }
    if (payload_len > BUZZ_FRAME_MAX_PAYLOAD) {
        return BUZZ_ERR_ARG;
    }
    size_t total = BUZZ_FRAME_HEADER_LEN + (size_t)payload_len + BUZZ_FRAME_CRC_LEN;
    if (out_cap < total) {
        return BUZZ_ERR_SIZE;
    }

    out[0] = BUZZ_MAGIC0;
    out[1] = BUZZ_MAGIC1;
    out[2] = BUZZ_VERSION_1;
    out[3] = type;
    out[4] = session;
    out[5] = flags;
    write_le16(out + 6, channel);
    write_le16(out + 8, payload_len);
    if (payload_len > 0) {
        memcpy(out + BUZZ_FRAME_HEADER_LEN, payload, payload_len);
    }

    // CRC 覆盖 version..payload（偏移 2 至 10+payload_len）
    uint16_t crc = buzz_crc16_update(BUZZ_CRC16_INIT, out + 2,
                                     BUZZ_FRAME_HEADER_LEN - 2 + (size_t)payload_len);
    write_le16(out + BUZZ_FRAME_HEADER_LEN + payload_len, crc);
    return (int)total;
}

int buzz_frag_encode(uint8_t *out, size_t out_cap, uint8_t type, uint8_t session,
                     uint16_t channel, uint16_t frag_index, uint16_t frag_count,
                     const uint8_t *data, uint16_t data_len)
{
    if (data == NULL && data_len > 0) {
        return BUZZ_ERR_NULL;
    }
    if (frag_count < 1u || frag_index >= frag_count || data_len > BUZZ_FRAG_MAX_DATA) {
        return BUZZ_ERR_ARG;
    }
    uint8_t payload[BUZZ_FRAME_MAX_PAYLOAD];
    write_le16(payload, frag_index);
    write_le16(payload + 2, frag_count);
    if (data_len > 0) {
        memcpy(payload + BUZZ_FRAG_HEADER_LEN, data, data_len);
    }
    return buzz_frame_encode(out, out_cap, type, session, BUZZ_FLAG_FRAG, channel, payload,
                             (uint16_t)(BUZZ_FRAG_HEADER_LEN + data_len));
}

int buzz_frag_parse(const buzz_frame_t *frame, uint16_t *frag_index, uint16_t *frag_count,
                    const uint8_t **data, uint16_t *data_len)
{
    if (frame == NULL || frag_index == NULL || frag_count == NULL || data == NULL ||
        data_len == NULL) {
        return BUZZ_ERR_NULL;
    }
    if ((frame->flags & BUZZ_FLAG_FRAG) == 0u || frame->length < BUZZ_FRAG_HEADER_LEN ||
        frame->payload == NULL) {
        return BUZZ_ERR_ARG;
    }
    *frag_index = read_le16(frame->payload);
    *frag_count = read_le16(frame->payload + 2);
    *data = frame->payload + BUZZ_FRAG_HEADER_LEN;
    *data_len = (uint16_t)(frame->length - BUZZ_FRAG_HEADER_LEN);
    return BUZZ_OK;
}

void buzz_parser_init(buzz_parser_t *parser, buzz_frame_cb_t cb, void *ctx)
{
    if (parser == NULL) {
        return;
    }
    memset(parser, 0, sizeof(*parser)); // state=BUZZ_PARSE_SYNC，缓冲清零
    parser->cb = cb;
    parser->ctx = ctx;
}

// 帧头收齐后的校验：version/type/flags/length 均通过才进入 PAYLOAD。
// 任一不满足（§10 未知 version/type/flags 位）→ DISCARD 按 length 跳整帧，保持流对齐。
static bool header_is_acceptable(const buzz_parser_t *parser)
{
    uint8_t version = parser->header[2];
    uint8_t type = parser->header[3];
    uint8_t flags = parser->header[5];
    uint16_t len = read_le16(parser->header + 8);

    if (version != BUZZ_VERSION_1 || !buzz_type_is_known(type)) {
        return false;
    }
    if ((flags & (uint8_t)~BUZZ_FLAG_FRAG) != 0u) {
        return false;
    }
    if (len > BUZZ_FRAME_MAX_PAYLOAD) {
        return false;
    }
    return true;
}

// 交付当前帧：CRC 校验通过则回调；随后无论成败都回到 SYNC 重新同步。
static void parser_finish_frame(buzz_parser_t *parser)
{
    uint16_t expect = parser->expect_len;
    uint16_t crc = buzz_crc16_update(BUZZ_CRC16_INIT, parser->header + 2,
                                     BUZZ_FRAME_HEADER_LEN - 2);
    crc = buzz_crc16_update(crc, parser->payload, expect);
    if (crc == read_le16(parser->crc) && parser->cb != NULL) {
        buzz_frame_t frame = {
            .version = parser->header[2],
            .type = parser->header[3],
            .session = parser->header[4],
            .flags = parser->header[5],
            .channel = read_le16(parser->header + 6),
            .length = expect,
            .payload = parser->payload,
        };
        parser->cb(&frame, parser->ctx);
    }
    parser->state = BUZZ_PARSE_SYNC;
    parser->header_len = 0;
}

int buzz_parser_feed(buzz_parser_t *parser, const uint8_t *data, size_t len)
{
    if (parser == NULL || (data == NULL && len > 0)) {
        return BUZZ_ERR_NULL;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        switch (parser->state) {
        case BUZZ_PARSE_SYNC:
            if (b == BUZZ_MAGIC0) {
                parser->header[0] = b;
                parser->header_len = 1;
                parser->state = BUZZ_PARSE_HEAD;
            }
            break;
        case BUZZ_PARSE_HEAD:
            parser->header[parser->header_len++] = b;
            if (parser->header_len == 2 && b != BUZZ_MAGIC1) {
                // 假魔数：当前字节可能是新魔数首字节，回退重同步
                parser->header_len = (b == BUZZ_MAGIC0) ? 1 : 0;
                if (parser->header_len == 0) {
                    parser->state = BUZZ_PARSE_SYNC;
                }
                break;
            }
            if (parser->header_len == BUZZ_FRAME_HEADER_LEN) {
                if (header_is_acceptable(parser)) {
                    parser->expect_len = read_le16(parser->header + 8);
                    parser->payload_len = 0;
                    parser->crc_len = 0;
                    parser->state = (parser->expect_len == 0u) ? BUZZ_PARSE_CRC
                                                               : BUZZ_PARSE_PAYLOAD;
                } else {
                    // 忽略该帧：按声明长度跳过 payload+crc（§10）
                    parser->discard_left =
                        (uint32_t)read_le16(parser->header + 8) + BUZZ_FRAME_CRC_LEN;
                    parser->state = BUZZ_PARSE_DISCARD;
                }
            }
            break;
        case BUZZ_PARSE_DISCARD:
            if (parser->discard_left > 0u) {
                parser->discard_left--;
            }
            if (parser->discard_left == 0u) {
                parser->state = BUZZ_PARSE_SYNC;
                parser->header_len = 0;
            }
            break;
        case BUZZ_PARSE_PAYLOAD:
            parser->payload[parser->payload_len++] = b;
            if (parser->payload_len == parser->expect_len) {
                parser->state = BUZZ_PARSE_CRC;
            }
            break;
        case BUZZ_PARSE_CRC:
            parser->crc[parser->crc_len++] = b;
            if (parser->crc_len == BUZZ_FRAME_CRC_LEN) {
                parser_finish_frame(parser);
            }
            break;
        default:
            // 状态非法：复位到同步起点，避免卡死
            parser->state = BUZZ_PARSE_SYNC;
            parser->header_len = 0;
            break;
        }
    }
    return BUZZ_OK;
}
