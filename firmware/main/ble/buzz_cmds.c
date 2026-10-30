// BuzzLink BLE 命令服务（M4.3）实现。线程模型与分层职责见 buzz_cmds.h 头注释。
#include "buzz_cmds.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h" // BSP_SD_MOUNT_POINT
#include "buzz_ble.h"
#include "buzz_dl.h"
#include "buzz_gatt.h"
#include "buzz_server.h"
#include "sys/power_axp.h"
#include "sys/rtc.h"
#include "sys/rtc_pcf.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "file_index.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "fw_version.h"
#include "host/ble_gap.h"
#include "nimble/ble.h" // BLE_ERR_REM_USER_CONN_TERM

static const char *TAG = "buzz_cmds";

// 服务任务优先级：低于音频采集（REC_TASK_PRIO 5），高于 REPL
#define BUZZ_CMDS_TASK_PRIO 4u
#define BUZZ_CMDS_TASK_STACK (4096u)
// 入向队列：定长槽位（GATT 单次写 ≤ MTU-3 ≤ 244B），满则丢弃并记日志
#define BUZZ_CMDS_Q_LEN 8u
#define BUZZ_CMDS_Q_SLOT 256u
// 空闲断开（D10）：无任何入向流量的累计秒数
#define BUZZ_CMDS_IDLE_TIMEOUT_S 60u

// ---------- 输入队列项 ----------

typedef enum {
    BUZZ_CMDS_ITEM_CMD = 0, // data = 中央设备写载荷（BuzzLink 字节流）
    BUZZ_CMDS_ITEM_EVT,     // data[0] = buzz_ble_evt_t
} buzz_cmds_item_kind_t;

typedef struct {
    uint8_t kind;
    uint16_t len;
    uint8_t data[BUZZ_CMDS_Q_SLOT];
} buzz_cmds_item_t;

// ---------- 模块静态状态（单连接单实例，零运行期 malloc） ----------

static buzz_server_t s_server; // 协议服务层实例（内嵌 ~5.5KB 缓冲）
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static uint32_t s_idle_sec; // 空闲计时（服务任务上下文；入向流量/出向进度复位，D10）

static int server_send_glue(void *ctx, const uint8_t *frame, size_t len);
static int control_handler(void *ctx, const buzz_frame_t *req, uint8_t *resp,
                           size_t resp_cap, int *err_code, const char **err_msg);
static int file_handler(void *ctx, const buzz_frame_t *req, uint8_t *resp,
                        size_t resp_cap, int *err_code, const char **err_msg);

// server IO 表：文件通道 → buzz_dl（DL_BEGIN/DL_END/DL_ABORT，M4.4）
static const buzz_server_io_t s_io = {
    .send = server_send_glue,
    .send_ctx = NULL,
    .control = control_handler,
    .control_ctx = NULL,
    .file = file_handler,
    .file_ctx = NULL,
};

// ---------- JSON 组装助手（snprintf 追加式；溢出 → overflow 标志） ----------

typedef struct {
    char *buf;
    size_t cap;
    size_t pos;
    bool overflow;
} jbuf_t;

static void jput(jbuf_t *j, const char *fmt, ...)
{
    if (j->overflow) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(j->buf + j->pos, j->cap - j->pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= j->cap - j->pos) {
        j->overflow = true; // 截断防御：容量按协议上限核算，实际不可达
        return;
    }
    j->pos += (size_t)n;
}

// ---------- file_index 的 FATFS/VFS 五件套实现 ----------

static void *vfs_open(void *ctx, const char *id)
{
    (void)ctx;
    char path[sizeof(BSP_SD_MOUNT_POINT) + 1u + BUZZ_FILE_ID_MAX_LEN];
    snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, id);
    return fopen(path, "rb");
}

// pread 语义：read_at 前总是 fseek 到绝对偏移，句柄无共享游标状态
static long vfs_read_at(void *ctx, void *fh, uint32_t off, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (fseek((FILE *)fh, (long)off, SEEK_SET) != 0) {
        return -1;
    }
    return (long)fread(buf, 1, len, (FILE *)fh);
}

static long vfs_size(void *ctx, void *fh)
{
    (void)ctx;
    if (fseek((FILE *)fh, 0, SEEK_END) != 0) {
        return -1;
    }
    return ftell((FILE *)fh); // TF 卡文件远小于 LONG_MAX
}

static void vfs_close(void *ctx, void *fh)
{
    (void)ctx;
    (void)fclose((FILE *)fh);
}

static long vfs_listdir(void *ctx, buzz_file_dirent_cb_t cb, void *cb_ctx)
{
    (void)ctx;
    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (dir == NULL) {
        return -1;
    }
    long n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        cb(cb_ctx, ent->d_name);
        n++;
    }
    closedir(dir);
    return n;
}

static const buzz_file_io_t s_file_io = {
    .ctx = NULL,
    .open = vfs_open,
    .read_at = vfs_read_at,
    .size = vfs_size,
    .close = vfs_close,
    .listdir = vfs_listdir,
};

// ---------- 业务 JSON 组装 ----------

// INFO（§8.1）：M5.4 起 battery/charging 取 AXP2101 平滑快照，未知电量报 -1；
// 存储字节数经 esp_vfs_fat_info（内部 f_getfree × 扇区大小）获取，未挂载按 0 报告。
static size_t build_info_json(char *buf, size_t cap)
{
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total_bytes, &free_bytes) != ESP_OK) {
        ESP_LOGW(TAG, "fat info failed, report storage 0");
    }
    // 电量快照：power 任务 1Hz 写的静态瞬态值（字宽读，无锁；显示级精度，
    // 单次撕裂读可容忍——刻意不引锁，避免与轮询任务互相拖住）
    uint8_t pct = 0xFFu;
    bool charging = false;
    power_axp_get_debug(NULL, NULL, &pct, &charging, NULL, NULL);

    jbuf_t j = {.buf = buf, .cap = cap, .pos = 0, .overflow = false};
    if (pct == 0xFFu) {
        jput(&j,
             "{\"model\":\"beecord-watch\",\"fwVersion\":\"%s\",\"protocol\":1,"
             "\"deviceName\":\"%s\",\"battery\":-1,\"charging\":false,"
             "\"storageTotal\":%llu,\"storageFree\":%llu}",
             FW_VERSION, buzz_ble_device_name(), (unsigned long long)total_bytes,
             (unsigned long long)free_bytes);
    } else {
        jput(&j,
             "{\"model\":\"beecord-watch\",\"fwVersion\":\"%s\",\"protocol\":1,"
             "\"deviceName\":\"%s\",\"battery\":%u,\"charging\":%s,"
             "\"storageTotal\":%llu,\"storageFree\":%llu}",
             FW_VERSION, buzz_ble_device_name(), (unsigned)pct,
             charging ? "true" : "false", (unsigned long long)total_bytes,
             (unsigned long long)free_bytes);
    }
    return j.overflow ? 0u : j.pos;
}

// 控制通道 in-payload 错误（§8.2/§8.3 错误形态：响应 type 不变、status=error；
// Studio 对 LIST_RESP/FILE_INFO_RESP 均按此解析）。返回 JSON 长度。
static size_t build_error_json(char *buf, size_t cap, int code, const char *message)
{
    jbuf_t j = {.buf = buf, .cap = cap, .pos = 0, .overflow = false};
    jput(&j, "{\"status\":\"error\",\"code\":%d,\"message\":\"%s\"}", code, message);
    return j.overflow ? 0u : j.pos;
}

// ---------- TIME_SET（§8.4 v0.2）：epochMs(UTC)+utcOffsetMin → 本地日历写 RTC ----------

static int handle_time_set(const char *json, char *resp, size_t resp_cap, int *err_code,
                           const char **err_msg)
{
    int64_t epoch_ms = 0;
    long offset_min = 0;
    int rc = buzz_json_get_i64(json, "epochMs", &epoch_ms);
    if (rc == BUZZ_JSON_ERR_NOT_FOUND) {
        return (int)build_error_json(resp, resp_cap, BUZZ_ERR_CODE_BAD_REQUEST,
                                     "missing epochMs");
    }
    if (rc != BUZZ_OK) {
        return (int)build_error_json(resp, resp_cap, BUZZ_ERR_CODE_BAD_REQUEST,
                                     "invalid epochMs");
    }
    rc = buzz_json_get_int(json, "utcOffsetMin", &offset_min);
    if (rc == BUZZ_JSON_ERR_NOT_FOUND) {
        return (int)build_error_json(resp, resp_cap, BUZZ_ERR_CODE_BAD_REQUEST,
                                     "missing utcOffsetMin");
    }
    if (rc != BUZZ_OK) {
        return (int)build_error_json(resp, resp_cap, BUZZ_ERR_CODE_BAD_REQUEST,
                                     "invalid utcOffsetMin");
    }

    rtc_datetime_t local;
    if (!rtc_time_set_convert(epoch_ms, (int)offset_min, &local)) {
        return (int)build_error_json(resp, resp_cap, BUZZ_ERR_CODE_BAD_REQUEST,
                                     "epochMs or utcOffsetMin out of range");
    }
    if (rtc_pcf_write(&local) != ESP_OK) {
        return (int)build_error_json(resp, resp_cap, BUZZ_ERR_CODE_INTERNAL,
                                     "rtc write failed");
    }
    const int n = snprintf(resp, resp_cap,
                           "{\"status\":\"ok\",\"epochMs\":%lld}",
                           (long long)epoch_ms);
    if (n < 0 || (size_t)n >= resp_cap) {
        *err_code = BUZZ_ERR_CODE_INTERNAL;
        *err_msg = "time_set resp overflow";
        return 0;
    }
    return n;
}

// ---------- 控制通道命令 ----------

// LIST_RESP（§8.2）。offset 缺省 0；limit 缺省/上限 16（§5.2）；offset > total
// 回 bad offset。翻页终止与 Studio SyncEngine 双条件兼容（total 对账或空页）。
static int handle_list(const char *json, char *resp, size_t resp_cap, int *err_code,
                       const char **err_msg)
{
    long offset = 0;
    long limit = 0; // 0 = 缺省 16（file_index 层落实）
    int rc = buzz_json_get_int(json, "offset", &offset);
    if (rc == BUZZ_JSON_ERR_MALFORMED || (rc == BUZZ_OK && (offset < 0 || offset > 0x7FFFFFFFL))) {
        *err_code = BUZZ_ERR_CODE_BAD_REQUEST;
        *err_msg = "bad request";
        return -1;
    }
    rc = buzz_json_get_int(json, "limit", &limit);
    if (rc == BUZZ_JSON_ERR_MALFORMED || (rc == BUZZ_OK && (limit < 0 || limit > 0x7FFFFFFFL))) {
        *err_code = BUZZ_ERR_CODE_BAD_REQUEST;
        *err_msg = "bad request";
        return -1;
    }

    buzz_file_entry_t entries[BUZZ_INDEX_PAGE_MAX];
    uint32_t total = 0;
    uint32_t count = 0;
    rc = buzz_file_index_list((uint32_t)offset, (uint32_t)limit, entries,
                              BUZZ_INDEX_PAGE_MAX, &total, &count);
    if (rc == BUZZ_INDEX_ERR_IO) {
        return (int)build_error_json(resp, resp_cap, BUZZ_ERR_CODE_STORAGE, "storage error");
    }
    if (rc != BUZZ_OK) {
        return (int)build_error_json(resp, resp_cap, BUZZ_ERR_CODE_INTERNAL, "internal");
    }
    if ((uint32_t)offset > total) {
        return (int)build_error_json(resp, resp_cap, BUZZ_ERR_CODE_BAD_REQUEST, "bad offset");
    }

    jbuf_t j = {.buf = resp, .cap = resp_cap, .pos = 0, .overflow = false};
    jput(&j, "{\"status\":\"ok\",\"total\":%lu,\"offset\":%lu,\"files\":[",
         (unsigned long)total, (unsigned long)offset);
    for (uint32_t i = 0; i < count; i++) {
        jput(&j, "%s{\"id\":\"%s\",\"durationMs\":%lu,\"size\":%lu}", i == 0 ? "" : ",",
             entries[i].id, (unsigned long)entries[i].durationMs,
             (unsigned long)entries[i].size);
    }
    jput(&j, "]}");
    if (j.overflow) {
        *err_code = BUZZ_ERR_CODE_INTERNAL;
        *err_msg = "internal";
        return -1;
    }
    return (int)j.pos;
}

// FILE_INFO_RESP（§8.3）。format 取值（与 Studio RecordingMeta.format 自由串
// 对齐，Sim 用 "ogg-opus"）：OGG → "ogg-opus"；WAV → "wav-pcm"（容器-编码命名）。
// 未找到/存储错走 in-payload error（code 4004/4500）。
static int handle_file_info(const char *json, char *resp, size_t resp_cap, int *err_code,
                            const char **err_msg)
{
    char id[BUZZ_FILE_ID_MAX_LEN + 1u];
    const int rc = buzz_json_get_str(json, "id", id, sizeof(id));
    if (rc != BUZZ_OK || id[0] == '\0' || strchr(id, '/') != NULL ||
        strchr(id, '\\') != NULL) {
        *err_code = BUZZ_ERR_CODE_BAD_REQUEST;
        *err_msg = "bad request";
        return -1;
    }

    const int64_t t0 = esp_timer_get_time();
    buzz_file_info_t info;
    const int irc = buzz_file_index_info(id, &info);
    const int64_t took_ms = (esp_timer_get_time() - t0) / 1000;
    if (took_ms > 500) {
        // 慢路径诊断：冷算 CRC（无缓存）的大文件在此留痕；稳态应只有暖缓存
        ESP_LOGI(TAG, "fileinfo id=%s took %u ms (size=%lu)", id, (unsigned)took_ms,
                 (unsigned long)info.size);
    }
    if (irc == BUZZ_INDEX_ERR_NOT_FOUND) {
        return (int)build_error_json(resp, resp_cap, BUZZ_ERR_CODE_NOT_FOUND,
                                     "file not found");
    }
    if (irc == BUZZ_INDEX_ERR_IO) {
        return (int)build_error_json(resp, resp_cap, BUZZ_ERR_CODE_STORAGE, "storage error");
    }
    if (irc != BUZZ_OK) {
        return (int)build_error_json(resp, resp_cap, BUZZ_ERR_CODE_INTERNAL, "internal");
    }

    jbuf_t j = {.buf = resp, .cap = resp_cap, .pos = 0, .overflow = false};
    jput(&j,
         "{\"status\":\"ok\",\"id\":\"%s\",\"size\":%lu,\"crc16\":%u,"
         "\"durationMs\":%lu,\"format\":\"%s\",\"sampleRate\":%lu,\"channels\":%lu}",
         id, (unsigned long)info.size, (unsigned)info.crc16,
         (unsigned long)info.durationMs, info.format, (unsigned long)info.sampleRate,
         (unsigned long)info.channels);
    if (j.overflow) {
        *err_code = BUZZ_ERR_CODE_INTERNAL;
        *err_msg = "internal";
        return -1;
    }
    return (int)j.pos;
}

// 控制通道分发（buzz_server 回调）：req payload 非 NUL 结尾，先拷入本地缓冲。
// 返回 ≥0 响应字节数；<0 由 server 层自动发 ERROR 帧（err_code/err_msg）。
static int control_handler(void *ctx, const buzz_frame_t *req, uint8_t *resp,
                           size_t resp_cap, int *err_code, const char **err_msg)
{
    (void)ctx;
    char json[BUZZ_SERVER_REQ_CAP + 1u];
    const size_t n = req->length < BUZZ_SERVER_REQ_CAP ? req->length : BUZZ_SERVER_REQ_CAP;
    memcpy(json, req->payload, n);
    json[n] = '\0';
    char *out = (char *)resp; // server 响应缓冲（resp_cap = BUZZ_SERVER_RESP_CAP）

    switch (req->type) {
    case BUZZ_TYPE_INFO_GET: {
        const size_t len = build_info_json(out, resp_cap);
        return len > 0u ? (int)len : -1;
    }
    case BUZZ_TYPE_LIST_REQ:
        return handle_list(json, out, resp_cap, err_code, err_msg);
    case BUZZ_TYPE_FILE_INFO_REQ:
        return handle_file_info(json, out, resp_cap, err_code, err_msg);
    case BUZZ_TYPE_TIME_SET_REQ:
        return handle_time_set(json, out, resp_cap, err_code, err_msg);
    case BUZZ_TYPE_FILE_DELETE_REQ:
        // P1 命令，v1 不支持：ERROR 帧 4003（§9）
        *err_code = BUZZ_ERR_CODE_FORBIDDEN;
        *err_msg = "delete not supported in v1";
        return -1;
    default:
        // 未知控制类型：server 分发层已拦截（response_type 映射为 0 静默忽略），
        // 此处仅防御
        *err_code = BUZZ_ERR_CODE_BAD_REQUEST;
        *err_msg = "bad request";
        return -1;
    }
}

// ---------- 出向发送（server 发送回调 → GATT notify） ----------

static int server_send_glue(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    if (len == 0u || len > 0xFFFFu) {
        return -1;
    }
    const int rc = buzz_gatt_notify_send(frame, (uint16_t)len);
    if (rc != 0) {
        ESP_LOGD(TAG, "notify failed rc=%d len=%u", rc, (unsigned)len);
        return -1; // server 层据此报 BUZZ_SERVER_ERR_SEND（断连/背压同型，pump 按连接态区分）
    }
    return 0;
}

// ---------- 文件通道（→ buzz_dl，M4.4） ----------

static int file_handler(void *ctx, const buzz_frame_t *req, uint8_t *resp,
                        size_t resp_cap, int *err_code, const char **err_msg)
{
    return buzz_dl_handle_request(ctx, req, resp, resp_cap, err_code, err_msg);
}

// ---------- server 复位与事件处理（服务任务上下文） ----------

// 复位 server 全部流状态（跨连接残留的半帧/分片必须清掉），并按当前 MTU 重设
// 分片上限（disconnect 后 buzz_ble_mtu() 回落 23 → 8，下次连接 MTU 事件再校准）。
static void server_reset(void)
{
    buzz_server_init(&s_server, &s_io);
    const uint16_t mtu = buzz_ble_mtu();
    if (mtu > 15u) {
        (void)buzz_server_set_max_payload(&s_server, (uint16_t)(mtu - 15u));
    }
}

static void push_info_proactive(void)
{
    char info[384];
    const size_t len = build_info_json(info, sizeof(info));
    if (len == 0u) {
        ESP_LOGE(TAG, "info json overflow");
        return;
    }
    // §6.1：订阅后主动推 INFO，session=0
    const int rc = buzz_server_send(&s_server, BUZZ_TYPE_INFO, 0u, BUZZ_CHANNEL_CONTROL,
                                    (const uint8_t *)info, len);
    if (rc != BUZZ_OK) {
        ESP_LOGW(TAG, "proactive INFO send failed rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "proactive INFO pushed (session=0, %u bytes)", (unsigned)len);
    }
}

static void handle_event(buzz_ble_evt_t evt)
{
    switch (evt) {
    case BUZZ_BLE_EVT_CONNECT:
        s_idle_sec = 0;
        server_reset();
        ESP_LOGI(TAG, "peer connected, server state reset");
        break;
    case BUZZ_BLE_EVT_DISCONNECT:
        buzz_dl_cancel(); // 断连立即作废未完成的传输（§7：全部未完成命令作废）
        server_reset();
        ESP_LOGI(TAG, "peer disconnected, server state reset");
        break;
    case BUZZ_BLE_EVT_MTU: {
        const uint16_t mtu = buzz_ble_mtu();
        if (mtu > 15u) {
            const uint16_t payload = (uint16_t)(mtu - 15u); // §3.3：MTU-3-12
            const int rc = buzz_server_set_max_payload(&s_server, payload);
            ESP_LOGI(TAG, "att_mtu=%u max_payload=%u set rc=%d", (unsigned)mtu,
                     (unsigned)payload, rc);
        }
        break;
    }
    case BUZZ_BLE_EVT_SUBSCRIBE:
        if (buzz_ble_notify_subscribed()) {
            push_info_proactive();
        }
        break;
    default:
        break;
    }
}

// ---------- host 任务上下文入口（只推队列，绝不碰 server/文件） ----------

static void on_gatt_write(const uint8_t *data, uint16_t len, void *ctx)
{
    (void)ctx;
    if (s_queue == NULL || len == 0u || len > BUZZ_CMDS_Q_SLOT) {
        ESP_LOGW(TAG, "gatt write dropped len=%u", (unsigned)len);
        return;
    }
    buzz_cmds_item_t item = {0};
    item.kind = BUZZ_CMDS_ITEM_CMD;
    item.len = len;
    memcpy(item.data, data, len);
    if (xQueueSend(s_queue, &item, 0) != pdPASS) {
        ESP_LOGW(TAG, "cmd queue full, dropped %u bytes", (unsigned)len);
    }
}

static void on_ble_evt(buzz_ble_evt_t evt, void *ctx)
{
    (void)ctx;
    if (s_queue == NULL) {
        return;
    }
    buzz_cmds_item_t item = {0};
    item.kind = BUZZ_CMDS_ITEM_EVT;
    item.len = 1u;
    item.data[0] = (uint8_t)evt;
    if (xQueueSend(s_queue, &item, 0) != pdPASS) {
        ESP_LOGW(TAG, "event queue full, dropped evt=%d", (int)evt);
    }
}

// ---------- 服务任务 ----------

static void buzz_cmds_task(void *arg)
{
    (void)arg;
    buzz_cmds_item_t item;
    ESP_LOGI(TAG, "service task started");
    for (;;) {
        // 下载进行中用 2ms 短滴答做背压轮询续发（TX 池由控制器完成包释放后
        // 重试入队）；平时 1s 滴答兼作空闲断开计时（D10）。
        const TickType_t wait =
            buzz_dl_busy() ? pdMS_TO_TICKS(2) : pdMS_TO_TICKS(1000);
        if (xQueueReceive(s_queue, &item, wait) == pdPASS) {
            if (item.kind == BUZZ_CMDS_ITEM_CMD) {
                s_idle_sec = 0; // 有入向流量即复位空闲计时（D10）
                const int rc = buzz_server_feed(&s_server, item.data, item.len);
                if (rc != BUZZ_OK) {
                    ESP_LOGW(TAG, "server feed rc=%d", rc);
                }
                // 命令处理完立即泵推流：DL_BEGIN 受理后马上出首块（流控在 pump 内）
                const int sent = buzz_dl_pump();
                if (sent != 0) {
                    s_idle_sec = 0; // 出向进度同样复位空闲计时（D10）
                }
            } else {
                handle_event((buzz_ble_evt_t)item.data[0]);
            }
            continue;
        }
        if (buzz_dl_busy()) {
            // 传输即活跃（D10）：不计空闲；泵一次尝试续发（ENOMEM 时本轮 0 进展，
            // 下个 2ms 滴答再试）。
            s_idle_sec = 0;
            const int sent = buzz_dl_pump();
            if (sent != 0) {
                s_idle_sec = 0;
            }
            continue;
        }
        if (!buzz_ble_is_connected()) {
            s_idle_sec = 0;
            continue;
        }
        if (++s_idle_sec >= BUZZ_CMDS_IDLE_TIMEOUT_S) {
            const uint16_t conn = buzz_ble_conn_handle();
            ESP_LOGI(TAG, "idle %us, terminate conn=%u", (unsigned)BUZZ_CMDS_IDLE_TIMEOUT_S,
                     (unsigned)conn);
            s_idle_sec = 0;
            const int rc = ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            if (rc != 0) {
                ESP_LOGW(TAG, "gap terminate rc=%d", rc);
            }
        }
    }
}

// ---------- 启动 ----------

esp_err_t buzz_cmds_init(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const int frc = buzz_file_index_init(&s_file_io);
    if (frc != BUZZ_OK) {
        ESP_LOGE(TAG, "file_index init failed rc=%d", frc);
        return ESP_FAIL;
    }
    buzz_server_init(&s_server, &s_io);
    if (buzz_dl_init(&s_server) != 0) {
        ESP_LOGE(TAG, "buzz_dl init failed");
        return ESP_FAIL;
    }

    s_queue = xQueueCreate(BUZZ_CMDS_Q_LEN, sizeof(buzz_cmds_item_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        return ESP_ERR_NO_MEM;
    }

    buzz_gatt_set_write_cb(on_gatt_write, NULL);
    buzz_ble_set_evt_cb(on_ble_evt, NULL);

    const BaseType_t ok = xTaskCreate(buzz_cmds_task, "buzz_cmds", BUZZ_CMDS_TASK_STACK,
                                      NULL, BUZZ_CMDS_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        buzz_gatt_set_write_cb(NULL, NULL);
        buzz_ble_set_evt_cb(NULL, NULL);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "buzz cmds service up (task prio %u, queue %ux%uB)", BUZZ_CMDS_TASK_PRIO,
             (unsigned)BUZZ_CMDS_Q_LEN, (unsigned)sizeof(buzz_cmds_item_t));
    return ESP_OK;
}
