// BuzzLink BLE 命令服务（M4.3）实现。线程模型与分层职责见 buzz_cmds.h 头注释。
#include "buzz_cmds.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "audio/buzz_recorder.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h" // BSP_SD_MOUNT_POINT
#include "buzz_ble.h"
#include "buzz_dl.h"
#include "buzz_gatt.h"
#include "buzz_sdcard.h"
#include "buzz_server.h"
#include "sys/beep.h"
#include "sys/power_axp.h"
#include "sys/rtc.h"
#include "sys/rtc_pcf.h"
#include "sys/settings.h"
#include "sys/sync_marks.h"
#include "ui/device_model.h"
#include "ui/screen_pwr.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
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
#define BUZZ_CMDS_TASK_STACK (8192u)
// 入向队列：定长槽位（GATT 单次写 ≤ MTU-3 ≤ 244B），满则丢弃并记日志
#define BUZZ_CMDS_Q_LEN 8u
#define BUZZ_CMDS_Q_SLOT 256u
// 空闲断开（D10）：无任何入向流量的累计秒数
#define BUZZ_CMDS_IDLE_TIMEOUT_S 60u

// ---------- 输入队列项 ----------

typedef enum {
    BUZZ_CMDS_ITEM_CMD = 0, // data = 中央设备写载荷（BuzzLink 字节流）
    BUZZ_CMDS_ITEM_EVT,     // data[0] = buzz_ble_evt_t
    BUZZ_CMDS_ITEM_REFRESH,
    BUZZ_CMDS_ITEM_SETTING,
    BUZZ_CMDS_ITEM_FORMAT,
} buzz_cmds_item_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t storage_busy;
    uint16_t len;
    uint8_t data[BUZZ_CMDS_Q_SLOT];
} buzz_cmds_item_t;

// ---------- 模块静态状态（单连接单实例，零运行期 malloc） ----------

static buzz_server_t s_server; // 协议服务层实例（内嵌 ~5.5KB 缓冲）
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static uint32_t s_idle_sec; // 空闲计时（服务任务上下文；入向流量/出向进度复位，D10）
static EXT_RAM_BSS_ATTR device_snapshot_t s_model_build;
static EXT_RAM_BSS_ATTR buzz_file_entry_t s_model_files[DEVICE_MODEL_MAX_FILES];
static atomic_uint s_refresh_queued;
static bool s_formatting;
static bool s_reject_storage_io;

static void server_reset(void);

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

static void *vfs_index_realloc(void *ctx, void *ptr, size_t size)
{
    (void)ctx;
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void vfs_index_free(void *ctx, void *ptr)
{
    (void)ctx;
    heap_caps_free(ptr);
}

static const buzz_file_io_t s_file_io = {
    .ctx = NULL,
    .open = vfs_open,
    .read_at = vfs_read_at,
    .size = vfs_size,
    .close = vfs_close,
    .listdir = vfs_listdir,
    .realloc = vfs_index_realloc,
    .free = vfs_index_free,
};

// ---------- 本地设备模型（唯一生产者：本服务任务） ----------

static void model_fill_common(void)
{
    s_model_build.ble_connected = buzz_ble_is_connected();
    s_model_build.ble_subscribed = buzz_ble_notify_subscribed();
    s_model_build.rec_quality = settings_get_u8(SETTINGS_REC_QUALITY);
    s_model_build.beep_enabled = settings_get_u8(SETTINGS_BEEP_ENABLED);
    s_model_build.screen_off_s = settings_get_u8(SETTINGS_SCREEN_OFF_S);

    buzz_dl_snapshot_t dl;
    buzz_dl_snapshot(&dl);
    s_model_build.transfer_state = DEVICE_TRANSFER_IDLE;
    s_model_build.transfer_id[0] = '\0';
    s_model_build.sent_chunks = 0;
    s_model_build.total_chunks = 0;
    if (dl.active) {
        s_model_build.transfer_state =
            dl.wait_end ? DEVICE_TRANSFER_WAIT_END : DEVICE_TRANSFER_SENDING;
        strncpy(s_model_build.transfer_id, dl.id, sizeof(s_model_build.transfer_id) - 1u);
        s_model_build.sent_chunks = dl.sent_chunks;
        s_model_build.total_chunks = dl.total_chunks;
    }
}

static void publish_notice(device_notice_t notice)
{
    device_model_snapshot(&s_model_build);
    s_model_build.notice_seq++;
    s_model_build.notice = notice;
    device_model_publish(&s_model_build);
}

static void model_note_time_set(const rtc_datetime_t *local)
{
    device_model_snapshot(&s_model_build);
    s_model_build.time_set_valid = true;
    s_model_build.time_year = local->year;
    s_model_build.time_month = local->month;
    s_model_build.time_day = local->day;
    s_model_build.time_hour = local->hour;
    s_model_build.time_minute = local->minute;
    device_model_publish(&s_model_build);
}

static void refresh_model(bool scan)
{
    device_model_snapshot(&s_model_build);
    model_fill_common();
    if (scan) {
        s_model_build.refreshing = true;
        device_model_publish(&s_model_build);
    }

    if (buzz_sdcard_mounted()) {
        if (!s_formatting) {
            s_model_build.storage_state = DEVICE_STORAGE_MOUNTED;
        }
        s_model_build.storage_error = 0;
        uint64_t total = 0;
        uint64_t free_bytes = 0;
        if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
            s_model_build.storage_total = total;
            s_model_build.storage_free = free_bytes;
        } else {
            s_model_build.storage_total = 0;
            s_model_build.storage_free = 0;
            s_model_build.storage_state = DEVICE_STORAGE_ERROR;
            s_model_build.storage_error = ESP_FAIL;
        }
    } else if (!s_formatting && s_model_build.storage_state != DEVICE_STORAGE_FORMAT_CONFIRM &&
               s_model_build.storage_state != DEVICE_STORAGE_FORMAT_FAILED) {
        s_model_build.storage_state = buzz_sdcard_last_error() == ESP_FAIL
                                          ? DEVICE_STORAGE_FORMAT_CANDIDATE
                                          : DEVICE_STORAGE_UNAVAILABLE;
        s_model_build.storage_error = (int32_t)buzz_sdcard_last_error();
        s_model_build.storage_total = 0;
        s_model_build.storage_free = 0;
        s_model_build.file_count = 0;
        s_model_build.total_count = 0;
        s_model_build.pending_count = 0;
    }

    if (scan && buzz_sdcard_mounted() && !s_formatting) {
        uint32_t total = 0;
        uint32_t count = 0;
        const int rc = buzz_file_index_list_all(s_model_files, DEVICE_MODEL_MAX_FILES,
                                                &total, &count);
        if (rc != BUZZ_OK) {
            ESP_LOGW(TAG, "model file scan failed rc=%d", rc);
            s_model_build.storage_state = DEVICE_STORAGE_ERROR;
            s_model_build.file_count = 0;
            s_model_build.total_count = 0;
            s_model_build.pending_count = 0;
        } else {
            const bool recording_active = buzz_recorder_state() != BUZZ_REC_STATE_IDLE;
            const char *active = recording_active ? buzz_recorder_current_path() : "";
            const char *slash = strrchr(active, '/');
            if (slash != NULL) {
                active = slash + 1;
            }
            uint16_t out = 0;
            for (uint32_t i = 0; i < count; i++) {
                if (device_model_file_is_active(recording_active, active, s_model_files[i].id)) {
                    continue;
                }
                device_file_t *dst = &s_model_build.files[out];
                memset(dst, 0, sizeof(*dst));
                strncpy(dst->id, s_model_files[i].id, sizeof(dst->id) - 1u);
                dst->size = s_model_files[i].size;
                dst->duration_ms = s_model_files[i].durationMs;
                dst->synced = sync_marks_has(dst->id);
                out++;
            }
            s_model_build.file_count = out;
            s_model_build.total_count = (uint16_t)(total > UINT16_MAX ? UINT16_MAX : total);
            device_model_recount(&s_model_build);
        }
    }
    s_model_build.refreshing = false;
    device_model_publish(&s_model_build);
}

static void handle_local_setting(buzz_local_setting_t setting)
{
    switch (setting) {
    case BUZZ_LOCAL_SETTING_SCREEN_OFF: {
        const uint8_t old = settings_get_u8(SETTINGS_SCREEN_OFF_S);
        const uint8_t next = old == 10u ? 30u : old == 30u ? 60u : old == 60u ? 0u : 10u;
        settings_set_u8(SETTINGS_SCREEN_OFF_S, next);
        screen_pwr_set_timeout_ms((uint32_t)next * 1000u);
        break;
    }
    case BUZZ_LOCAL_SETTING_QUALITY: {
        const uint8_t next = settings_get_u8(SETTINGS_REC_QUALITY) == 0u ? 1u : 0u;
        settings_set_u8(SETTINGS_REC_QUALITY, next);
        buzz_recorder_set_bitrate(next == 1u ? 24000 : 0);
        break;
    }
    case BUZZ_LOCAL_SETTING_BEEP: {
        const uint8_t next = settings_get_u8(SETTINGS_BEEP_ENABLED) == 0u ? 1u : 0u;
        settings_set_u8(SETTINGS_BEEP_ENABLED, next);
        beep_set_enabled(next != 0u);
        break;
    }
    default:
        return;
    }
    const bool save_ok = settings_last_save_ok();
    refresh_model(false);
    if (!save_ok) {
        publish_notice(DEVICE_NOTICE_SETTING_SAVE_FAILED);
    }
}

static void handle_local_format(buzz_format_action_t action)
{
    device_model_snapshot(&s_model_build);
    const bool busy = buzz_recorder_state() != BUZZ_REC_STATE_IDLE || buzz_dl_busy();
    device_storage_state_t state = s_model_build.storage_state;
    device_format_event_t evt;
    if (action == BUZZ_FORMAT_CANCEL) {
        evt = DEVICE_FORMAT_EVT_CANCEL;
    } else if (action == BUZZ_FORMAT_CONFIRM) {
        if (state == DEVICE_STORAGE_FORMAT_CANDIDATE ||
            state == DEVICE_STORAGE_FORMAT_FAILED) {
            state = device_format_transition(state, DEVICE_FORMAT_EVT_REQUEST, busy);
        }
        evt = DEVICE_FORMAT_EVT_CONFIRM;
    } else {
        evt = DEVICE_FORMAT_EVT_REQUEST;
    }
    const device_storage_state_t next = device_format_transition(state, evt, busy);
    if (busy && (action == BUZZ_FORMAT_CONFIRM || action == BUZZ_FORMAT_REQUEST)) {
        publish_notice(DEVICE_NOTICE_BUSY);
        return;
    }
    if (next == DEVICE_STORAGE_FORMATTING) {
        s_formatting = true;
        server_reset();
    }
    s_model_build.storage_state = next;
    device_model_publish(&s_model_build);
    if (next != DEVICE_STORAGE_FORMATTING) {
        return;
    }

    refresh_model(false);
    const esp_err_t ret = buzz_sdcard_format();
    s_formatting = false;
    device_model_snapshot(&s_model_build);
    state = device_format_transition(DEVICE_STORAGE_FORMATTING,
                                     ret == ESP_OK ? DEVICE_FORMAT_EVT_SUCCEEDED
                                                   : DEVICE_FORMAT_EVT_FAILED,
                                     false);
    s_model_build.storage_state = state;
    s_model_build.storage_error = (int32_t)ret;
    device_model_publish(&s_model_build);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(ret));
        publish_notice(DEVICE_NOTICE_FORMAT_FAILED);
        return;
    }

    const int clear_rc = sync_marks_clear();
    (void)buzz_file_index_init(&s_file_io);
    refresh_model(true);
    publish_notice(clear_rc == 0 ? DEVICE_NOTICE_FORMAT_DONE
                                 : DEVICE_NOTICE_FORMAT_SAVE_FAILED);
}

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
             "\"deviceName\":\"%s\",\"deviceID\":\"%s\",\"battery\":-1,\"charging\":false,"
             "\"storageTotal\":%llu,\"storageFree\":%llu}",
             FW_VERSION, buzz_ble_device_name(), buzz_ble_device_id(),
             (unsigned long long)total_bytes, (unsigned long long)free_bytes);
    } else {
        jput(&j,
             "{\"model\":\"beecord-watch\",\"fwVersion\":\"%s\",\"protocol\":1,"
             "\"deviceName\":\"%s\",\"deviceID\":\"%s\",\"battery\":%u,\"charging\":%s,"
             "\"storageTotal\":%llu,\"storageFree\":%llu}",
             FW_VERSION, buzz_ble_device_name(), buzz_ble_device_id(), (unsigned)pct,
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
    settings_set_time_context((int)offset_min);
    if (!settings_last_save_ok()) {
        ESP_LOGW(TAG, "time context persist failed; runtime time remains valid");
    }
    model_note_time_set(&local);
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

static void jput_recording_meta(jbuf_t *j, const char *recording_id, bool has_started_at,
                                int64_t started_at_epoch_ms, int32_t utc_offset_min,
                                rec_meta_time_quality_t quality)
{
    if (recording_id == NULL || recording_id[0] == '\0') {
        return;
    }
    jput(j, ",\"recordingID\":\"%s\"", recording_id);
    if (has_started_at) {
        jput(j, ",\"startedAtEpochMs\":%lld,\"startedAtUtcOffsetMin\":%ld",
             (long long)started_at_epoch_ms, (long)utc_offset_min);
    }
    jput(j, ",\"timeQuality\":\"%s\"", rec_meta_time_quality_name(quality));
}

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
        jput(&j, "%s{\"id\":\"%s\"", i == 0 ? "" : ",", entries[i].id);
        jput_recording_meta(&j, entries[i].recording_id, entries[i].has_started_at,
                            entries[i].started_at_epoch_ms,
                            entries[i].started_at_utc_offset_min, entries[i].time_quality);
        jput(&j, ",\"durationMs\":%lu,\"size\":%lu}",
             (unsigned long)entries[i].durationMs, (unsigned long)entries[i].size);
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
    jput(&j, "{\"status\":\"ok\",\"id\":\"%s\"", id);
    jput_recording_meta(&j, info.recording_id, info.has_started_at,
                        info.started_at_epoch_ms, info.started_at_utc_offset_min,
                        info.time_quality);
    jput(&j,
         ",\"size\":%lu,\"crc16\":%u,\"durationMs\":%lu,\"format\":\"%s\","
         "\"sampleRate\":%lu,\"channels\":%lu}",
         (unsigned long)info.size, (unsigned)info.crc16,
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

    if (s_reject_storage_io &&
        (req->type == BUZZ_TYPE_LIST_REQ || req->type == BUZZ_TYPE_FILE_INFO_REQ)) {
        return (int)build_error_json(out, resp_cap, BUZZ_ERR_CODE_BUSY, "formatting busy");
    }

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
    if (s_reject_storage_io && req->type == BUZZ_TYPE_DL_BEGIN_REQ) {
        if (resp_cap < 13u) {
            return 0;
        }
        memset(resp, 0, 13u);
        resp[0] = BUZZ_STATUS_BUSY;
        return 13;
    }
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
    refresh_model(false);
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
    item.storage_busy = __atomic_load_n(&s_formatting, __ATOMIC_ACQUIRE);
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

// ---------- 本地 UI / REPL 请求入口 ----------

static esp_err_t post_local_item(buzz_cmds_item_kind_t kind, uint8_t value)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    buzz_cmds_item_t item = {0};
    item.kind = (uint8_t)kind;
    item.len = 1u;
    item.data[0] = value;
    return xQueueSend(s_queue, &item, 0) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t buzz_cmds_request_refresh(void)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_exchange_explicit(&s_refresh_queued, 1u, memory_order_acq_rel) != 0u) {
        return ESP_OK;
    }
    const esp_err_t ret = post_local_item(BUZZ_CMDS_ITEM_REFRESH, 0u);
    if (ret != ESP_OK) {
        atomic_store_explicit(&s_refresh_queued, 0u, memory_order_release);
    }
    return ret;
}

esp_err_t buzz_cmds_cycle_setting(buzz_local_setting_t setting)
{
    if (setting > BUZZ_LOCAL_SETTING_BEEP) {
        return ESP_ERR_INVALID_ARG;
    }
    return post_local_item(BUZZ_CMDS_ITEM_SETTING, (uint8_t)setting);
}

esp_err_t buzz_cmds_format_action(buzz_format_action_t action)
{
    if (action > BUZZ_FORMAT_CONFIRM) {
        return ESP_ERR_INVALID_ARG;
    }
    return post_local_item(BUZZ_CMDS_ITEM_FORMAT, (uint8_t)action);
}

// ---------- 服务任务 ----------

static void buzz_cmds_task(void *arg)
{
    (void)arg;
    buzz_cmds_item_t item;
    int64_t last_progress_us = 0;
    ESP_LOGI(TAG, "service task started");
    refresh_model(true);
    for (;;) {
        const TickType_t wait =
            buzz_dl_busy() ? pdMS_TO_TICKS(2) : pdMS_TO_TICKS(1000);
        if (xQueueReceive(s_queue, &item, wait) == pdPASS) {
            if (item.kind == BUZZ_CMDS_ITEM_CMD) {
                s_idle_sec = 0;
                const bool was_busy = buzz_dl_busy();
                s_reject_storage_io = item.storage_busy != 0u;
                const int rc = buzz_server_feed(&s_server, item.data, item.len);
                s_reject_storage_io = false;
                if (rc != BUZZ_OK) {
                    ESP_LOGW(TAG, "server feed rc=%d", rc);
                }
                const int sent = buzz_dl_pump();
                if (sent != 0) {
                    s_idle_sec = 0;
                }
                refresh_model(was_busy && !buzz_dl_busy());
            } else if (item.kind == BUZZ_CMDS_ITEM_EVT) {
                handle_event((buzz_ble_evt_t)item.data[0]);
            } else if (item.kind == BUZZ_CMDS_ITEM_REFRESH) {
                atomic_store_explicit(&s_refresh_queued, 0u, memory_order_release);
                refresh_model(true);
            } else if (item.kind == BUZZ_CMDS_ITEM_SETTING) {
                handle_local_setting((buzz_local_setting_t)item.data[0]);
            } else if (item.kind == BUZZ_CMDS_ITEM_FORMAT) {
                handle_local_format((buzz_format_action_t)item.data[0]);
            }
            continue;
        }
        if (buzz_dl_busy()) {
            s_idle_sec = 0;
            const int sent = buzz_dl_pump();
            const int64_t now = esp_timer_get_time();
            if (now - last_progress_us >= 100000 || sent < 0) {
                refresh_model(false);
                last_progress_us = now;
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
    device_model_init();
    atomic_init(&s_refresh_queued, 0u);
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
