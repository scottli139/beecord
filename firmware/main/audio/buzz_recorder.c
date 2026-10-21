// Beecord 录音采集任务实现（M2 WAV 管线，M3 双格式，M5.3 可靠性+热键支撑）。
// 采集循环：esp_codec_dev_read → L/R 混单声道 → Ogg（rec_pipe）或 WAV 追加写。
// 非 OK 读取计数并跳过（不写假数据）；连续失败超阈值判定链路故障中止并回报。
// M5.3：暂停期读块照常读但丢弃（mic 链路保温、计时冻结）；电平经 rec_meter；
// 每 5s 经 rec_journal_save 落盘 journal（断电恢复依赖）；写满（ENOSPC）单独事件。
#include "buzz_recorder.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "rec_journal_esp.h"
#include "rec_meter.h"
#include "rec_pipe.h"
#include "wav_writer.h"

static const char *TAG = "buzz_recorder";

// 采集任务参数（M2 定案；M3 栈 6KB→8KB：Opus pseudostack 虽落 PSRAM，
// 编码调用链与页缓冲仍留余量；M5.3 功能扩展均在循环内小步进，栈水位 M5.6 审计复核）
#define REC_TASK_PRIO 5
#define REC_TASK_STACK 8192
#define REC_TASK_NAME "buzz_rec"
#define REC_READ_BUF_BYTES 4096u                        // 4KB 双声道读缓冲 = 64ms@16kHz
#define REC_CMD_TIMEOUT_MS 10000u                       // start/stop/pause 握手超时
#define REC_MAX_CONSEC_READ_ERRS 10u                    // 连续读失败阈值：判定链路故障中止
#define REC_PATH_MAX 64u
#define REC_ACK_MSG_MAX 96u
#define REC_MAX_MARKS 64u                               // 会话标记点上限（journal 同宽）
#define REC_JOURNAL_PERIOD_MS 5000u                     // 每 5s flush + journal 落盘（M5_PLAN D8）
#define REC_FLUSH_DISCARD_LIMIT 200u                    // 暂停期连续丢弃读块上限（防假死）

typedef enum {
    REC_CMD_START = 0,
    REC_CMD_STOP,
    REC_CMD_PAUSE, // M5.3
    REC_CMD_RESUME,
    REC_CMD_MARK,
} rec_cmd_type_t;

// 控制方 → 采集任务的命令：START 携带封装格式
typedef struct {
    rec_cmd_type_t type;
    buzz_rec_fmt_t fmt;
} rec_cmd_t;

// 采集任务 → 控制方的一次性应答：错误码 + 人类可读说明（路径/错误原因）
typedef struct {
    esp_err_t err;
    char message[REC_ACK_MSG_MAX];
} rec_ack_t;

static QueueHandle_t s_cmd_q; // 控制方 → 采集任务（depth 1）
static QueueHandle_t s_ack_q; // 采集任务 → 控制方（depth 1）
static TaskHandle_t s_task;
static esp_codec_dev_handle_t s_mic;
static buzz_rec_state_t s_state = BUZZ_REC_STATE_IDLE;
static uint32_t s_next_index; // 启动预扫的下一可用序号（开录前会重扫）
static uint8_t s_pcm_buf[REC_READ_BUF_BYTES];
static int s_bitrate_bps;     // 0 = 默认（BUZZ_REC_PIPE_BITRATE）

// M5.3：事件回调（采集任务上下文触发）
static buzz_rec_event_cb_t s_evt_cb;
static void *s_evt_ctx;

// M5.3：会话实时统计（原子字宽，任意任务轮询读）
static volatile uint32_t s_elapsed_ms;
static volatile uint8_t s_level;
static volatile uint64_t s_bytes_written;
static uint32_t s_mark_ms[REC_MAX_MARKS]; // 非 volatile：仅采集任务写，读取在 LVGL 任务——
                                          // 配合 mark_count 的读后写序，竞态最多丢末标记，可接受
static volatile uint32_t s_mark_count;
static volatile uint8_t s_inst_level;  // 最近块未平滑目标档（rec_meter_last_target 镜像）
static volatile uint8_t s_win_peak;    // 自上次读取以来的窗口峰值（UI 200ms 采样取峰）
static char s_current_path[REC_PATH_MAX];
static rec_meter_t s_meter;

static void emit(buzz_rec_evt_t evt, const char *message, int32_t value)
{
    if (s_evt_cb != NULL) {
        s_evt_cb(evt, message != NULL ? message : "", value, s_evt_ctx);
    }
}

// OGG 会话的静态资源（M3）：管线对象内嵌 ~5KB + 4KB 页缓冲，启动期即占用，
// 采集循环内零 malloc。serial 用文件序号（整流一致即可）。
static buzz_rec_pipe_t s_pipe;
static uint8_t s_ogg_page_buf[BUZZ_REC_PIPE_PAGE_BUF_SIZE];

// OGG 页落盘 sink：页回调 fwrite 直写本会话文件；短写区分 ENOSPC（写满）与普通错误
typedef struct {
    FILE *fp;
    uint32_t write_errs;
    bool disk_full;
} ogg_sink_t;

static void ogg_page_to_file(const uint8_t *page, uint32_t page_len, void *ctx)
{
    ogg_sink_t *sink = (ogg_sink_t *)ctx;
    errno = 0;
    if (fwrite(page, 1, page_len, sink->fp) != page_len) {
        sink->write_errs++;
        if (errno == ENOSPC) {
            sink->disk_full = true;
        }
    }
}

// ---------- 文件序号扫描 ----------

// 扫挂载点根目录，返回 REC_%04d.WAV/.OGG 两种后缀的全局最大序号（无则 0）。
// 两种格式共用序号池：混合格式目录里序号仍单调递增。
static uint32_t scan_max_rec_index(void)
{
    uint32_t max_idx = 0;
    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (dir == NULL) {
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (strncmp(name, "REC_", 4) != 0) {
            continue;
        }
        const char *num_start = name + 4;
        char *num_end = NULL;
        unsigned long v = strtoul(num_start, &num_end, 10);
        if (num_end == num_start || num_end == NULL || v == 0 || v > 99999ul) {
            continue;
        }
        if (strcasecmp(num_end, ".WAV") != 0 && strcasecmp(num_end, ".OGG") != 0) {
            continue;
        }
        if (v > max_idx) {
            max_idx = (uint32_t)v;
        }
    }
    closedir(dir);
    return max_idx;
}

// ---------- 混单声道 ----------

// L/R 交错 int16 → 单声道：(int32)(l + r) / 2 中间量防溢出，结果就地写回缓冲前半
static void downmix_stereo_to_mono(int16_t *buf, int frames)
{
    for (int i = 0; i < frames; i++) {
        int32_t l = buf[2 * i];
        int32_t r = buf[2 * i + 1];
        buf[i] = (int16_t)((l + r) / 2);
    }
}

// ---------- 会话公共状态（M5.3） ----------

// 会话上下文：两个 session 函数共享的暂停/统计/journal 逻辑载体
typedef struct {
    rec_ack_t ack;
    uint32_t read_errs;
    uint32_t consec_errs;
    bool aborted;
    bool disk_full;
    uint32_t last_journal_ms;   // 上次 journal 落盘的会话时刻
    uint32_t discard_in_pause;  // 暂停期连续丢弃计数（防假死）
    uint32_t mono_samples;      // 每读块混单声道后的采样数
} session_ctx_t;

// 每读块后更新统计：电平（rec_meter）+ 时长 + 字节（字节由 sink/写路径另行累计）
// M5.3 电平排查诊断：原始块 RMS/峰值观测。每 32 块（~2s）打一条——
// 用于区分"原始 PCM 动态恒定（前端/硬件问题）"与"仪表链把动态抹平"。
// 宿主单测不覆盖此胶（ESP 侧）；判定表见 M5.3 修复汇报。
static uint32_t s_diag_last_rms;
static uint32_t s_diag_min_rms;
static uint32_t s_diag_max_rms;
static uint32_t s_diag_blocks;

static void diag_reset(void)
{
    s_diag_last_rms = 0;
    s_diag_min_rms = UINT32_MAX;
    s_diag_max_rms = 0;
    s_diag_blocks = 0;
}

static void diag_feed(const int16_t *mono, uint32_t nsamples)
{
    if (mono == NULL || nsamples == 0) {
        return;
    }
    int64_t acc = 0;
    uint32_t peak = 0;
    for (uint32_t i = 0; i < nsamples; i++) {
        const int32_t v = mono[i];
        acc += v * v;
        const uint32_t a = (uint32_t)(v < 0 ? -v : v);
        if (a > peak) {
            peak = a;
        }
    }
    const uint32_t rms = (uint32_t)sqrt((double)acc / (double)nsamples);
    s_diag_last_rms = rms;
    if (rms < s_diag_min_rms) {
        s_diag_min_rms = rms;
    }
    if (rms > s_diag_max_rms) {
        s_diag_max_rms = rms;
    }
    s_diag_blocks++;
    if (s_diag_blocks % 32u == 0u) {
        ESP_LOGI(TAG,
                 "meter-diag: blk rms=%" PRIu32 " peak=%" PRIu32
                 " | session rms min=%" PRIu32 " max=%" PRIu32,
                 rms, peak, s_diag_min_rms, s_diag_max_rms);
    }
}

static void session_update_stats(session_ctx_t *c, const int16_t *mono, bool paused)
{
    uint8_t lvl = rec_meter_feed(&s_meter, mono, paused ? 0u : c->mono_samples);
    s_level = lvl;
    const uint8_t inst = rec_meter_last_target(&s_meter); // 本块未平滑目标
    s_inst_level = inst;
    if (!paused) {
        if (inst > s_win_peak) {
            s_win_peak = inst;
        }
        diag_feed(mono, c->mono_samples);
    }
    if (!paused) {
        s_elapsed_ms += c->mono_samples * 1000u / 16000u;
    }
}

// 每 5s flush + journal 落盘（M5_PLAN D8：断电后开机恢复依赖此数据）
static void session_journal_tick(session_ctx_t *c, buzz_rec_fmt_t fmt)
{
    if (s_elapsed_ms - c->last_journal_ms < REC_JOURNAL_PERIOD_MS) {
        return;
    }
    c->last_journal_ms = s_elapsed_ms;
    rec_journal_t j = { 0 };
    snprintf(j.file, sizeof(j.file), "%s", s_current_path);
    j.fmt = (uint8_t)fmt;
    // journal.bytes = 文件总字节（截断目标）：OGG 的 bytes_out 含全部页；
    // WAV 计数只累计数据段，须补 44 字节头（恢复侧头回填与之一致）
    j.bytes = s_bytes_written + (fmt == BUZZ_REC_FMT_WAV ? BUZZ_WAV_HEADER_LEN : 0u);
    j.elapsed_ms = s_elapsed_ms;
    j.marks = s_mark_count;
    uint32_t n = s_mark_count < REC_MAX_MARKS ? s_mark_count : REC_MAX_MARKS;
    for (uint32_t i = 0; i < n; i++) {
        j.mark_ms[i] = s_mark_ms[i];
    }
    esp_err_t ret = rec_journal_save(&j); // ESP 侧胶（rec_journal_esp.c）
    if (ret != ESP_OK) {
        // journal 失败不中止录音（主数据优先），计数走日志
        ESP_LOGW(TAG, "journal save failed: %s", esp_err_to_name(ret));
    }
}

// 循环顶部命令处理：返回 true = 收到停止
static bool session_poll_cmd(session_ctx_t *c)
{
    rec_cmd_t cmd;
    if (xQueueReceive(s_cmd_q, &cmd, 0) != pdTRUE) {
        return false;
    }
    switch (cmd.type) {
    case REC_CMD_STOP:
        return true;
    case REC_CMD_PAUSE: {
        rec_ack_t pack = { .err = ESP_OK, .message = { 0 } };
        if (s_state == BUZZ_REC_STATE_RECORDING) {
            s_state = BUZZ_REC_STATE_PAUSED;
            emit(BUZZ_REC_EVT_PAUSED, NULL, 0);
        } else {
            pack.err = ESP_ERR_INVALID_STATE;
        }
        xQueueSend(s_ack_q, &pack, 0);
        break;
    }
    case REC_CMD_RESUME: {
        rec_ack_t rack = { .err = ESP_OK, .message = { 0 } };
        if (s_state == BUZZ_REC_STATE_PAUSED) {
            s_state = BUZZ_REC_STATE_RECORDING;
            emit(BUZZ_REC_EVT_RESUMED, NULL, 0);
        } else {
            rack.err = ESP_ERR_INVALID_STATE;
        }
        xQueueSend(s_ack_q, &rack, 0);
        break;
    }
    case REC_CMD_MARK: {
        // 标记只在录音态有效（暂停态拒绝）；应答携带当前标记数
        rec_ack_t mack = { .err = ESP_OK, .message = { 0 } };
        if (s_state != BUZZ_REC_STATE_RECORDING) {
            mack.err = ESP_ERR_INVALID_STATE;
        } else if (s_mark_count >= REC_MAX_MARKS) {
            mack.err = ESP_ERR_NO_MEM; // 标记满（64 个，会议场景远够用）
        } else {
            uint32_t idx = s_mark_count;
            s_mark_ms[idx] = s_elapsed_ms;
            s_mark_count = idx + 1u;
            emit(BUZZ_REC_EVT_MARKED, NULL, (int32_t)(idx + 1u));
        }
        xQueueSend(s_ack_q, &mack, 0);
        break;
    }
    default:
        break;
    }
    return false;
}

// ---------- 一次录音会话（WAV 路径，M2 原逻辑 + M5.3 扩展） ----------

static void rec_session_wav(void)
{
    session_ctx_t c = { .ack = { .err = ESP_OK, .message = { 0 } } };

    // 开录前重扫：卡在他机写过或其它途径新增文件时序号仍单调
    s_next_index = scan_max_rec_index() + 1u;
    char path[REC_PATH_MAX];
    snprintf(path, sizeof(path), "%s/REC_%04" PRIu32 ".WAV", BSP_SD_MOUNT_POINT,
             s_next_index);

    buzz_wav_writer_t wav;
    int wret = buzz_wav_open(&wav, path);
    if (wret != BUZZ_OK) {
        c.ack.err = ESP_FAIL;
        snprintf(c.ack.message, sizeof(c.ack.message), "open %s failed (wav err %d)",
                 path, wret);
        ESP_LOGE(TAG, "%s", c.ack.message);
        xQueueSend(s_ack_q, &c.ack, 0);
        return;
    }

    s_state = BUZZ_REC_STATE_RECORDING;
    snprintf(s_current_path, sizeof(s_current_path), "%s", path);
    rec_meter_reset(&s_meter);
    diag_reset();
    s_elapsed_ms = 0;
    s_level = 0;
    s_bytes_written = 0;
    s_mark_count = 0;
    c.mono_samples = (uint32_t)(sizeof(s_pcm_buf) / 2u / sizeof(int16_t));
    ESP_LOGI(TAG, "recording started: %s", path);

    // 开工应答立即发出（start 在等）；收尾应答在会话结束时另发。
    snprintf(c.ack.message, sizeof(c.ack.message), "%s", path);
    xQueueSend(s_ack_q, &c.ack, 0);
    c.ack.err = ESP_OK;
    c.ack.message[0] = '\0';
    emit(BUZZ_REC_EVT_STARTED, path, 0);

    for (;;) {
        if (session_poll_cmd(&c)) {
            break;
        }
        bool paused = (s_state == BUZZ_REC_STATE_PAUSED);

        int ret = esp_codec_dev_read(s_mic, s_pcm_buf, (int)sizeof(s_pcm_buf));
        if (ret != ESP_CODEC_DEV_OK) {
            c.read_errs++;
            c.consec_errs++;
            if (c.consec_errs >= REC_MAX_CONSEC_READ_ERRS) {
                c.ack.err = ESP_FAIL;
                snprintf(c.ack.message, sizeof(c.ack.message),
                         "read failed %d times, aborted (last codec err %d)",
                         (int)REC_MAX_CONSEC_READ_ERRS, ret);
                ESP_LOGE(TAG, "%s", c.ack.message);
                c.aborted = true;
                break;
            }
            continue; // 该块丢弃，不写假数据
        }
        c.consec_errs = 0;

        downmix_stereo_to_mono((int16_t *)s_pcm_buf, (int)c.mono_samples);
        session_update_stats(&c, (const int16_t *)s_pcm_buf, paused);
        if (paused) {
            // 暂停期丢弃读块（计时已冻结）；连续丢弃计数防 I2S 假死观测
            c.discard_in_pause++;
            continue;
        }
        c.discard_in_pause = 0;

        const uint32_t mono_bytes = c.mono_samples * sizeof(int16_t);
        wret = buzz_wav_write(&wav, s_pcm_buf, mono_bytes);
        if (wret != BUZZ_OK) {
            c.ack.err = ESP_FAIL;
            snprintf(c.ack.message, sizeof(c.ack.message), "write failed (wav err %d)",
                     wret);
            ESP_LOGE(TAG, "%s", c.ack.message);
            c.aborted = true;
            c.disk_full = (errno == ENOSPC);
            break;
        }
        s_bytes_written += mono_bytes;
        session_journal_tick(&c, BUZZ_REC_FMT_WAV);
    }

    wret = buzz_wav_close(&wav);
    if (wret != BUZZ_OK) {
        c.ack.err = ESP_FAIL;
        snprintf(c.ack.message, sizeof(c.ack.message), "close failed (wav err %d)", wret);
        ESP_LOGE(TAG, "%s", c.ack.message);
    }
    s_state = BUZZ_REC_STATE_IDLE;
    s_level = 0; // 会话结束电平归零（idle 不再保持末值）

    const uint64_t data_bytes = wav.data_bytes;
    const uint64_t duration_ms = data_bytes * 1000u / BUZZ_WAV_BYTE_RATE;
    ESP_LOGI(TAG, "recording %s: %s bytes=%" PRIu64 " dur=%" PRIu64 "ms read_errs=%"
             PRIu32, c.aborted ? "aborted" : "done", path, data_bytes, duration_ms,
             c.read_errs);

    if (!c.aborted) {
        rec_journal_clear();
    }
    if (c.ack.err == ESP_OK) {
        snprintf(c.ack.message, sizeof(c.ack.message), "%s", path);
        emit(BUZZ_REC_EVT_STOPPED, path, 0);
    } else {
        emit(c.disk_full ? BUZZ_REC_EVT_AUTO_STOPPED : BUZZ_REC_EVT_ERROR, c.ack.message,
             0);
    }
    xQueueSend(s_ack_q, &c.ack, 0);
}

// ---------- 一次录音会话（OGG 路径，M3 管线 + M5.3 扩展） ----------

static void rec_session_ogg(void)
{
    session_ctx_t c = { .ack = { .err = ESP_OK, .message = { 0 } } };

    // 开录前重扫：卡在他机写过或其它途径新增文件时序号仍单调
    s_next_index = scan_max_rec_index() + 1u;
    char path[REC_PATH_MAX];
    snprintf(path, sizeof(path), "%s/REC_%04" PRIu32 ".OGG", BSP_SD_MOUNT_POINT,
             s_next_index);

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        c.ack.err = ESP_FAIL;
        snprintf(c.ack.message, sizeof(c.ack.message), "open %s failed", path);
        ESP_LOGE(TAG, "%s", c.ack.message);
        xQueueSend(s_ack_q, &c.ack, 0);
        return;
    }

    ogg_sink_t sink = { .fp = fp, .write_errs = 0, .disk_full = false };
    const int bitrate = s_bitrate_bps > 0 ? s_bitrate_bps : BUZZ_REC_PIPE_BITRATE;
    int pret = buzz_rec_pipe_init_ex(&s_pipe, s_next_index, bitrate, s_ogg_page_buf,
                                     sizeof(s_ogg_page_buf), ogg_page_to_file, &sink);
    if (pret != BUZZ_OK) {
        fclose(fp);
        c.ack.err = ESP_FAIL;
        snprintf(c.ack.message, sizeof(c.ack.message), "ogg session init failed (err %d)",
                 pret);
        ESP_LOGE(TAG, "%s", c.ack.message);
        xQueueSend(s_ack_q, &c.ack, 0);
        return;
    }

    s_state = BUZZ_REC_STATE_RECORDING;
    snprintf(s_current_path, sizeof(s_current_path), "%s", path);
    rec_meter_reset(&s_meter);
    diag_reset();
    s_elapsed_ms = 0;
    s_level = 0;
    s_bytes_written = 0;
    s_mark_count = 0;
    c.mono_samples = (uint32_t)(sizeof(s_pcm_buf) / 2u / sizeof(int16_t));
    c.last_journal_ms = 0;
    ESP_LOGI(TAG, "recording started (ogg, %d bps): %s", bitrate, path);

    // 开工应答立即发出；收尾应答在会话结束时另发。应答内容只回路径，统计走日志
    snprintf(c.ack.message, sizeof(c.ack.message), "%s", path);
    xQueueSend(s_ack_q, &c.ack, 0);
    c.ack.err = ESP_OK;
    c.ack.message[0] = '\0';
    emit(BUZZ_REC_EVT_STARTED, path, 0);

    for (;;) {
        if (session_poll_cmd(&c)) {
            break;
        }
        bool paused = (s_state == BUZZ_REC_STATE_PAUSED);

        int ret = esp_codec_dev_read(s_mic, s_pcm_buf, (int)sizeof(s_pcm_buf));
        if (ret != ESP_CODEC_DEV_OK) {
            c.read_errs++;
            c.consec_errs++;
            if (c.consec_errs >= REC_MAX_CONSEC_READ_ERRS) {
                c.ack.err = ESP_FAIL;
                snprintf(c.ack.message, sizeof(c.ack.message),
                         "read failed %d times, aborted (last codec err %d)",
                         (int)REC_MAX_CONSEC_READ_ERRS, ret);
                ESP_LOGE(TAG, "%s", c.ack.message);
                c.aborted = true;
                break;
            }
            continue; // 该块丢弃，不写假数据
        }
        c.consec_errs = 0;

        downmix_stereo_to_mono((int16_t *)s_pcm_buf, (int)c.mono_samples);
        session_update_stats(&c, (const int16_t *)s_pcm_buf, paused);
        if (paused) {
            c.discard_in_pause++;
            continue; // 丢弃读块，编码管线不进帧
        }
        c.discard_in_pause = 0;

        pret = buzz_rec_pipe_feed(&s_pipe, (const int16_t *)s_pcm_buf, c.mono_samples);
        if (pret != BUZZ_OK) {
            c.ack.err = ESP_FAIL;
            snprintf(c.ack.message, sizeof(c.ack.message), "ogg feed failed (err %d)",
                     pret);
            ESP_LOGE(TAG, "%s", c.ack.message);
            c.aborted = true;
            break;
        }
        if (sink.write_errs > 0u) {
            c.disk_full = sink.disk_full;
            c.ack.err = ESP_FAIL;
            snprintf(c.ack.message, sizeof(c.ack.message),
                     c.disk_full ? "storage full" : "write failed (ogg page)");
            ESP_LOGE(TAG, "%s", c.ack.message);
            c.aborted = true;
            break;
        }
        s_bytes_written = s_pipe.stats.bytes_out;
        session_journal_tick(&c, BUZZ_REC_FMT_OGG);
    }

    pret = buzz_rec_pipe_finish(&s_pipe); // 尾帧丢弃 + EOS 封页
    if (pret != BUZZ_OK) {
        c.ack.err = ESP_FAIL;
        snprintf(c.ack.message, sizeof(c.ack.message), "ogg finish failed (err %d)", pret);
        ESP_LOGE(TAG, "%s", c.ack.message);
    }
    if (fclose(fp) != 0 && c.ack.err == ESP_OK) {
        c.ack.err = ESP_FAIL;
        snprintf(c.ack.message, sizeof(c.ack.message), "close failed");
        ESP_LOGE(TAG, "%s", c.ack.message);
    }
    s_state = BUZZ_REC_STATE_IDLE;
    s_level = 0; // 会话结束电平归零（idle 不再保持末值）

    const uint64_t bytes_out = s_pipe.stats.bytes_out;
    const uint64_t duration_ms = (uint64_t)s_pipe.stats.frames * 20u; // 20ms/帧
    ESP_LOGI(TAG,
             "recording %s: %s frames=%" PRIu32 " dur=%" PRIu64
             "ms bytes=%" PRIu64 " read_errs=%" PRIu32 " encode_errs=%" PRIu32,
             c.aborted ? "aborted" : "done", path, s_pipe.stats.frames, duration_ms,
             bytes_out, c.read_errs, s_pipe.stats.encode_errs);

    if (!c.aborted) {
        rec_journal_clear();
    }
    if (c.ack.err == ESP_OK) {
        // 成功应答只回路径（详细统计已在上方日志）
        snprintf(c.ack.message, sizeof(c.ack.message), "%s", path);
        emit(BUZZ_REC_EVT_STOPPED, path, 0);
    } else {
        emit(c.disk_full ? BUZZ_REC_EVT_AUTO_STOPPED : BUZZ_REC_EVT_ERROR, c.ack.message,
             0);
    }
    xQueueSend(s_ack_q, &c.ack, 0);
}

static void rec_session_run(buzz_rec_fmt_t fmt)
{
    if (fmt == BUZZ_REC_FMT_OGG) {
        rec_session_ogg();
    } else {
        rec_session_wav();
    }
}

// ---------- 采集任务 ----------

static void rec_task_main(void *arg)
{
    (void)arg;
    for (;;) {
        rec_cmd_t cmd;
        xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY);
        if (cmd.type == REC_CMD_START) {
            rec_session_run(cmd.fmt);
        }
        // 空闲态收到的其它命令直接丢弃（控制方已先行查状态给出提示）
    }
}

// ---------- 公共 API ----------

esp_err_t buzz_recorder_init(esp_codec_dev_handle_t mic)
{
    if (mic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_mic = mic;
    rec_meter_init(&s_meter, NULL); // 缺了它 decay_per_feed=0 电平永不退档（实机"条等高"根因）
    s_cmd_q = xQueueCreate(1, sizeof(rec_cmd_t));
    s_ack_q = xQueueCreate(1, sizeof(rec_ack_t));
    if (s_cmd_q == NULL || s_ack_q == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreate(rec_task_main, REC_TASK_NAME, REC_TASK_STACK, NULL,
                                REC_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    s_current_path[0] = '\0';
    s_next_index = scan_max_rec_index() + 1u;
    ESP_LOGI(TAG, "ready, next index %04" PRIu32, s_next_index);
    return ESP_OK;
}

esp_err_t buzz_recorder_start(buzz_rec_fmt_t fmt)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == BUZZ_REC_STATE_RECORDING || s_state == BUZZ_REC_STATE_PAUSED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (fmt != BUZZ_REC_FMT_OGG && fmt != BUZZ_REC_FMT_WAV) {
        return ESP_ERR_INVALID_ARG;
    }
    // 清掉可能残留的过期应答（如上次会话读失败中止时无人收取的收尾应答），
    // 避免把旧 ack 误当本次 start 的开工应答
    xQueueReset(s_ack_q);
    rec_cmd_t cmd = { .type = REC_CMD_START, .fmt = fmt };
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    rec_ack_t ack;
    if (xQueueReceive(s_ack_q, &ack, pdMS_TO_TICKS(REC_CMD_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (ack.err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", ack.message);
    }
    return ack.err;
}

esp_err_t buzz_recorder_stop(void)
{
    if (s_task == NULL || s_state == BUZZ_REC_STATE_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    rec_cmd_t cmd = { .type = REC_CMD_STOP, .fmt = BUZZ_REC_FMT_OGG };
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    rec_ack_t ack;
    if (xQueueReceive(s_ack_q, &ack, pdMS_TO_TICKS(REC_CMD_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (ack.err == ESP_OK) {
        ESP_LOGI(TAG, "stopped: %s", ack.message);
    }
    return ack.err;
}

// M5.3：pause/resume/mark 走同一对命令/应答队列（互斥：调用方不得与
// start/stop 并发——UI 控制任务串行下发，天然满足）
static esp_err_t rec_simple_cmd(rec_cmd_type_t type)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xQueueReset(s_ack_q);
    rec_cmd_t cmd = { .type = type, .fmt = BUZZ_REC_FMT_OGG };
    if (xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    rec_ack_t ack;
    if (xQueueReceive(s_ack_q, &ack, pdMS_TO_TICKS(REC_CMD_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ack.err;
}

esp_err_t buzz_recorder_pause(void)
{
    if (s_state != BUZZ_REC_STATE_RECORDING) {
        return ESP_ERR_INVALID_STATE;
    }
    return rec_simple_cmd(REC_CMD_PAUSE);
}

esp_err_t buzz_recorder_resume(void)
{
    if (s_state != BUZZ_REC_STATE_PAUSED) {
        return ESP_ERR_INVALID_STATE;
    }
    return rec_simple_cmd(REC_CMD_RESUME);
}

esp_err_t buzz_recorder_mark(void)
{
    if (s_state != BUZZ_REC_STATE_RECORDING) {
        return ESP_ERR_INVALID_STATE;
    }
    return rec_simple_cmd(REC_CMD_MARK);
}

void buzz_recorder_set_event_cb(buzz_rec_event_cb_t cb, void *ctx)
{
    s_evt_cb = cb;
    s_evt_ctx = ctx;
}

buzz_rec_state_t buzz_recorder_state(void)
{
    return s_state;
}

uint32_t buzz_recorder_elapsed_ms(void)
{
    return s_elapsed_ms;
}

uint8_t buzz_recorder_level(void)
{
    return s_level;
}

uint64_t buzz_recorder_bytes_written(void)
{
    return s_bytes_written;
}

uint32_t buzz_recorder_mark_count(void)
{
    return s_mark_count;
}

uint32_t buzz_recorder_mark_ms(uint32_t idx)
{
    if (idx >= s_mark_count || idx >= REC_MAX_MARKS) {
        return 0;
    }
    return s_mark_ms[idx];
}

const char *buzz_recorder_current_path(void)
{
    return s_current_path;
}

uint8_t buzz_recorder_level_peak(void)
{
    uint8_t peak = s_win_peak;
    s_win_peak = s_inst_level; // 读后续窗从当前瞬时档起算
    return peak;
}

void buzz_recorder_meter_diag(uint32_t *last_rms, uint32_t *min_rms, uint32_t *max_rms)
{
    if (last_rms != NULL) {
        *last_rms = s_diag_last_rms;
    }
    if (min_rms != NULL) {
        *min_rms = s_diag_min_rms == UINT32_MAX ? 0u : s_diag_min_rms;
    }
    if (max_rms != NULL) {
        *max_rms = s_diag_max_rms;
    }
}

void buzz_recorder_set_bitrate(int bitrate_bps)
{
    s_bitrate_bps = bitrate_bps;
}

int buzz_recorder_get_bitrate(void)
{
    return s_bitrate_bps;
}
