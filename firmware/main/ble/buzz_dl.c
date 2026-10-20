// BuzzLink 文件下载推流（M4.4）实现。状态机/流控/线程模型见 buzz_dl.h 头注释。
#include "buzz_dl.h"

#include <string.h>

#include "buzz_ble.h"
#include "buzz_proto.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "file_index.h"

#include "audio/buzz_recorder.h" // D8：录音互斥

static const char *TAG = "buzz_dl";

// DL_CHUNK payload = [seq u16][data ≤ chunkDataLen]；chunkDataLen ≤ 224（D3），
// 故发送缓冲定长 2+224，零 malloc 且永不触发分片（2+chunkDataLen ≤ max_payload）。
#define BUZZ_DL_CHUNK_BUF_LEN (2u + BUZZ_CHUNK_DATA_LEN_DEFAULT)

typedef enum {
    BUZZ_DL_STATE_IDLE = 0,
    BUZZ_DL_STATE_STREAMING, // 还有块未入队
    BUZZ_DL_STATE_WAIT_END,  // 全部块已入队，等 DL_END_REQ 校验
} buzz_dl_state_t;

static buzz_server_t *s_server; // buzz_dl_init 绑定的协议服务实例
static buzz_dl_state_t s_state;
static void *s_fh; // 打开的文件句柄（file_index 注入的 io 表操作）
static char s_id[BUZZ_FILE_ID_MAX_LEN + 1u];
static uint32_t s_size;
static uint32_t s_total_chunks;
static uint16_t s_chunk_len; // DL_BEGIN_RESP 声明的 chunkDataLen（D3）
static uint32_t s_seq;       // 下一块序号
static uint8_t s_session;    // DL_BEGIN 的 session，DL_CHUNK 沿用（§3.4）
static uint16_t s_crc16;     // BEGIN 携带的整文件 CRC（END 回执同一值）
static int64_t s_start_us;   // BEGIN 受理时刻（吞吐统计）
static uint8_t s_chunk_buf[BUZZ_DL_CHUNK_BUF_LEN];

static void wr16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// ---------- 传输收尾 ----------

static void close_transfer(void)
{
    const buzz_file_io_t *io = buzz_file_index_io();
    if (s_fh != NULL && io != NULL) {
        io->close(io->ctx, s_fh);
    }
    s_fh = NULL;
    s_state = BUZZ_DL_STATE_IDLE;
}

// END/ABORT 时打印吞吐：size/耗时/KB/s
static void log_throughput(const char *how)
{
    const double secs = (double)(esp_timer_get_time() - s_start_us) / 1e6;
    const double kbs = secs > 0.0 ? ((double)s_size / 1024.0) / secs : 0.0;
    ESP_LOGI(TAG, "%s id=%s size=%lu chunks=%u seq_sent=%u in %.2fs (%.1f KB/s)", how,
             s_id, (unsigned long)s_size, (unsigned)s_total_chunks, (unsigned)s_seq,
             secs, kbs);
}

// ---------- 请求解析 ----------

// [idLen u8][id…] 提取：长度自洽、idLen ≤ 32、无路径分隔符。
// 成功返回 BUZZ_OK 并 NUL 拷贝到 out；否则返回 BUZZ_ERR_ARG。
static int parse_id_payload(const buzz_frame_t *req, char *out, size_t out_cap)
{
    if (req->length < 1u) {
        return BUZZ_ERR_ARG;
    }
    const uint8_t id_len = req->payload[0];
    if (id_len == 0u || id_len > BUZZ_FILE_ID_MAX_LEN ||
        (uint32_t)id_len + 1u != req->length || out_cap < (size_t)id_len + 1u) {
        return BUZZ_ERR_ARG;
    }
    memcpy(out, req->payload + 1u, id_len);
    out[id_len] = '\0';
    if (strchr(out, '/') != NULL || strchr(out, '\\') != NULL) {
        return BUZZ_ERR_ARG; // 路径穿越防线
    }
    return BUZZ_OK;
}

// ---------- DL_BEGIN ----------

static int resp_begin(uint8_t *resp, size_t cap, uint8_t status, uint16_t chunk_len,
                      uint32_t total_chunks, uint32_t size, uint16_t crc16)
{
    if (cap < 13u) {
        return 0; // 契约不可能：resp_cap = BUZZ_SERVER_RESP_CAP
    }
    resp[0] = status;
    wr16le(resp + 1u, chunk_len);
    wr32le(resp + 3u, total_chunks);
    wr32le(resp + 7u, size);
    wr16le(resp + 11u, crc16);
    return 13;
}

static int handle_begin(const buzz_frame_t *req, uint8_t *resp, size_t resp_cap)
{
    char id[BUZZ_FILE_ID_MAX_LEN + 1u];
    if (parse_id_payload(req, id, sizeof(id)) != BUZZ_OK) {
        return resp_begin(resp, resp_cap, BUZZ_STATUS_BAD_REQUEST, 0u, 0u, 0u, 0u);
    }
    if (s_state != BUZZ_DL_STATE_IDLE) {
        return resp_begin(resp, resp_cap, BUZZ_STATUS_BUSY, 0u, 0u, 0u, 0u);
    }
    if (buzz_recorder_state() == BUZZ_REC_STATE_RECORDING) {
        return resp_begin(resp, resp_cap, BUZZ_STATUS_BUSY, 0u, 0u, 0u, 0u); // D8
    }

    const buzz_file_io_t *io = buzz_file_index_io();
    if (io == NULL) {
        return resp_begin(resp, resp_cap, BUZZ_STATUS_STORAGE_ERROR, 0u, 0u, 0u, 0u);
    }
    void *fh = io->open(io->ctx, id);
    if (fh == NULL) {
        return resp_begin(resp, resp_cap, BUZZ_STATUS_NOT_FOUND, 0u, 0u, 0u, 0u);
    }
    const long fsize = io->size(io->ctx, fh);
    if (fsize < 0 || (unsigned long)fsize > 0xFFFFFFFFul) {
        io->close(io->ctx, fh);
        return resp_begin(resp, resp_cap, BUZZ_STATUS_STORAGE_ERROR, 0u, 0u, 0u, 0u);
    }

    // 整文件 CRC：经 file_index 缓存入口（FILE_INFO 与 DL_BEGIN 共享），
    // 冷算读全文件（16KB 块流式），热算零 IO。
    uint16_t crc16 = 0;
    const int crc_rc = buzz_file_index_crc16(id, (uint32_t)fsize, &crc16);
    if (crc_rc != BUZZ_OK) { // NOT_FOUND（size 不符）/ IO 错统一归存储错误
        io->close(io->ctx, fh);
        ESP_LOGW(TAG, "begin crc failed id=%s rc=%d", id, crc_rc);
        return resp_begin(resp, resp_cap, BUZZ_STATUS_STORAGE_ERROR, 0u, 0u, 0u, 0u);
    }

    // chunkDataLen（D3）：min(224, att_mtu-15-2)；max_payload = att_mtu-15，
    // 故 DL_CHUNK payload = 2+chunkDataLen ≤ max_payload，永不触发分片。
    uint32_t chunk_len = (uint32_t)buzz_ble_mtu() - 15u - 2u;
    if ((int32_t)chunk_len <= 0) {
        chunk_len = 1u; // att_mtu < 18 不存在（BLE 最小 23），防御
    }
    if (chunk_len > BUZZ_CHUNK_DATA_LEN_DEFAULT) {
        chunk_len = BUZZ_CHUNK_DATA_LEN_DEFAULT;
    }
    const uint32_t total_chunks =
        (uint32_t)fsize == 0u ? 0u : ((uint32_t)fsize + chunk_len - 1u) / chunk_len;

    s_fh = fh;
    strncpy(s_id, id, sizeof(s_id) - 1u);
    s_id[sizeof(s_id) - 1u] = '\0';
    s_size = (uint32_t)fsize;
    s_total_chunks = total_chunks;
    s_chunk_len = (uint16_t)chunk_len;
    s_seq = 0u;
    s_session = req->session;
    s_crc16 = crc16;
    s_start_us = esp_timer_get_time();
    s_state = total_chunks == 0u ? BUZZ_DL_STATE_WAIT_END : BUZZ_DL_STATE_STREAMING;
    ESP_LOGI(TAG, "begin id=%s size=%lu chunks=%u chunkLen=%u crc16=%u session=%u",
             s_id, (unsigned long)s_size, (unsigned)total_chunks, (unsigned)chunk_len,
             (unsigned)crc16, (unsigned)req->session);
    return resp_begin(resp, resp_cap, BUZZ_STATUS_OK, (uint16_t)chunk_len, total_chunks,
                      (uint32_t)fsize, crc16);
}

// ---------- DL_END / DL_ABORT ----------

// DL_END_RESP：[status u8][fileCrc16 u16]
static int resp_end(uint8_t *resp, size_t cap, uint8_t status, uint16_t crc16)
{
    if (cap < 3u) {
        return 0;
    }
    resp[0] = status;
    wr16le(resp + 1u, crc16);
    return 3;
}

// 公共校验：有活动传输且 id 匹配。返回 BUZZ_OK 或相应 status 字节。
static uint8_t match_active_transfer(const buzz_frame_t *req, char *id, size_t id_cap)
{
    if (s_state == BUZZ_DL_STATE_IDLE) {
        return BUZZ_STATUS_BAD_REQUEST;
    }
    if (parse_id_payload(req, id, id_cap) != BUZZ_OK || strcmp(id, s_id) != 0) {
        return BUZZ_STATUS_BAD_REQUEST;
    }
    return BUZZ_STATUS_OK;
}

static int handle_end(const buzz_frame_t *req, uint8_t *resp, size_t resp_cap)
{
    char id[BUZZ_FILE_ID_MAX_LEN + 1u];
    const uint8_t match = match_active_transfer(req, id, sizeof(id));
    if (match != BUZZ_STATUS_OK || s_state != BUZZ_DL_STATE_WAIT_END) {
        // 无传输 / id 不符 / 块未发完即请求结束：一律 bad_request
        return resp_end(resp, resp_cap, BUZZ_STATUS_BAD_REQUEST, 0u);
    }
    log_throughput("done");
    const uint16_t crc16 = s_crc16;
    close_transfer();
    return resp_end(resp, resp_cap, BUZZ_STATUS_OK, crc16);
}

static int handle_abort(const buzz_frame_t *req, uint8_t *resp, size_t resp_cap)
{
    char id[BUZZ_FILE_ID_MAX_LEN + 1u];
    const uint8_t match = match_active_transfer(req, id, sizeof(id));
    if (match != BUZZ_STATUS_OK) {
        return resp_end(resp, resp_cap, BUZZ_STATUS_BAD_REQUEST, 0u);
    }
    log_throughput("aborted");
    const uint16_t crc16 = s_crc16;
    close_transfer();
    return resp_end(resp, resp_cap, BUZZ_STATUS_ABORTED, crc16);
}

// ---------- 公开 API ----------

int buzz_dl_init(buzz_server_t *server)
{
    if (server == NULL) {
        return -1; // ESP_ERR_INVALID_ARG 的轻量等价（调用方已记日志惯例）
    }
    s_server = server;
    close_transfer();
    return 0;
}

int buzz_dl_handle_request(void *ctx, const buzz_frame_t *req, uint8_t *resp,
                           size_t resp_cap, int *err_code, const char **err_msg)
{
    (void)ctx;
    (void)err_code; // 文件通道错误在 payload 编 status 字节，不走 ERROR 帧
    (void)err_msg;
    switch (req->type) {
    case BUZZ_TYPE_DL_BEGIN_REQ:
        return handle_begin(req, resp, resp_cap);
    case BUZZ_TYPE_DL_END_REQ:
        return handle_end(req, resp, resp_cap);
    case BUZZ_TYPE_DL_ABORT:
        return handle_abort(req, resp, resp_cap);
    default:
        return 0; // server 分发层已拦截未知 type；防御性空响应不应到达
    }
}

int buzz_dl_pump(void)
{
    if (s_state != BUZZ_DL_STATE_STREAMING) {
        return 0;
    }
    const buzz_file_io_t *io = buzz_file_index_io();
    if (io == NULL) {
        close_transfer();
        return -1;
    }
    int sent = 0;
    while (s_seq < s_total_chunks) {
        const uint32_t off = s_seq * (uint32_t)s_chunk_len;
        uint32_t want = s_size - off;
        if (want > (uint32_t)s_chunk_len) {
            want = (uint32_t)s_chunk_len;
        }
        const long n = io->read_at(io->ctx, s_fh, off, s_chunk_buf + 2u, want);
        if (n != (long)want) { // 短读/读错：文件在传输中被破坏的极端情形，作废
            ESP_LOGE(TAG, "read failed seq=%u off=%lu want=%lu got=%ld, transfer aborted",
                     (unsigned)s_seq, (unsigned long)off, (unsigned long)want, n);
            close_transfer();
            return -1;
        }
        wr16le(s_chunk_buf, (uint16_t)s_seq);
        const int rc =
            buzz_server_send(s_server, BUZZ_TYPE_DL_CHUNK, s_session, BUZZ_CHANNEL_FILE,
                             s_chunk_buf, 2u + (size_t)n);
        if (rc != BUZZ_OK) {
            if (buzz_ble_is_connected()) {
                // TX 池耗尽（ENOMEM 被发送胶合归一为 SEND 失败）：背压挂起，
                // 等 BLE_GAP_EVENT_NOTIFY_TX 事件泵醒续发（D4）。
                ESP_LOGD(TAG, "tx pool full at seq=%u, back off", (unsigned)s_seq);
                return sent;
            }
            ESP_LOGW(TAG, "chunk send rc=%d (link lost), transfer aborted", rc);
            close_transfer();
            return -1;
        }
        s_seq++;
        sent++;
    }
    ESP_LOGI(TAG, "all %u chunks queued, wait DL_END", (unsigned)s_total_chunks);
    s_state = BUZZ_DL_STATE_WAIT_END;
    return sent;
}

void buzz_dl_cancel(void)
{
    if (s_state != BUZZ_DL_STATE_IDLE) {
        ESP_LOGI(TAG, "transfer cancelled id=%s at seq=%u/%u", s_id, (unsigned)s_seq,
                 (unsigned)s_total_chunks);
    }
    close_transfer();
}

bool buzz_dl_busy(void)
{
    return s_state != BUZZ_DL_STATE_IDLE;
}
