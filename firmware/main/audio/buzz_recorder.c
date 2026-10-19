// Beecord 录音采集任务实现（M2）。采集循环：esp_codec_dev_read（内部 1000ms 超时、
// 只回状态码不回字节数）→ L/R 交错 int16 混单声道（int32 中间量防溢出）→ wav 追加写。
// 非 OK 读取计数并跳过该块（不写假数据）；连续失败超过阈值判定链路故障，中止录音并回报。
#include "buzz_recorder.h"

#include <dirent.h>
#include <inttypes.h>
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

#include "wav_writer.h"

static const char *TAG = "buzz_recorder";

// 采集任务参数（M2_AUDIO_PLAN §4 定案）
#define REC_TASK_PRIO 5
#define REC_TASK_STACK 6144
#define REC_TASK_NAME "buzz_rec"
#define REC_READ_BUF_BYTES 4096u                        // 4KB 双声道读缓冲 = 1024 帧 = 64ms@16kHz
#define REC_CMD_TIMEOUT_MS 10000u                       // start/stop 握手超时
#define REC_MAX_CONSEC_READ_ERRS 10u                    // 连续读失败阈值：判定链路故障中止
#define REC_PATH_MAX 64u
#define REC_ACK_MSG_MAX 96u

typedef enum {
    REC_CMD_START = 0,
    REC_CMD_STOP,
} rec_cmd_t;

// 采集任务 → REPL 的一次性应答：错误码 + 人类可读说明（路径/错误原因）
typedef struct {
    esp_err_t err;
    char message[REC_ACK_MSG_MAX];
} rec_ack_t;

static QueueHandle_t s_cmd_q;  // REPL → 采集任务（depth 1）
static QueueHandle_t s_ack_q;  // 采集任务 → REPL（depth 1）
static TaskHandle_t s_task;
static esp_codec_dev_handle_t s_mic;
static buzz_rec_state_t s_state = BUZZ_REC_STATE_IDLE;
static uint32_t s_next_index; // 启动预扫的下一可用序号（开录前会重扫）
static uint8_t s_pcm_buf[REC_READ_BUF_BYTES];

// ---------- 文件序号扫描 ----------

// 扫挂载点根目录，返回 REC_%04d.WAV 的最大序号（无则 0）
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
        if (strcasecmp(num_end, ".WAV") != 0) {
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

// ---------- 一次录音会话 ----------

static void rec_session_run(void)
{
    rec_ack_t ack = { .err = ESP_OK, .message = { 0 } };

    // 开录前重扫：卡在他机写过或其它途径新增文件时序号仍单调
    s_next_index = scan_max_rec_index() + 1u;
    char path[REC_PATH_MAX];
    snprintf(path, sizeof(path), "%s/REC_%04" PRIu32 ".WAV", BSP_SD_MOUNT_POINT,
             s_next_index);

    buzz_wav_writer_t wav;
    int wret = buzz_wav_open(&wav, path);
    if (wret != BUZZ_OK) {
        ack.err = ESP_FAIL;
        snprintf(ack.message, sizeof(ack.message), "open %s failed (wav err %d)",
                 path, wret);
        ESP_LOGE(TAG, "%s", ack.message);
        xQueueSend(s_ack_q, &ack, 0);
        return;
    }

    s_state = BUZZ_REC_STATE_RECORDING;
    uint32_t read_errs = 0;
    uint32_t write_errs = 0;
    uint32_t consec_errs = 0;
    bool aborted = false;
    ESP_LOGI(TAG, "recording started: %s", path);

    // 开工应答立即发出（buzz_recorder_start 在等）；收尾应答在会话结束时另发，
    // 由 buzz_recorder_stop 收取。应答内容只回路径，统计走日志
    snprintf(ack.message, sizeof(ack.message), "%s", path);
    xQueueSend(s_ack_q, &ack, 0);
    ack.err = ESP_OK;
    ack.message[0] = '\0';

    for (;;) {
        // 挂起的 stop 命令优先处理（stop 到达时正在读的一帧由下一次循环收尾）
        rec_cmd_t cmd;
        if (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE && cmd == REC_CMD_STOP) {
            break;
        }

        int ret = esp_codec_dev_read(s_mic, s_pcm_buf, (int)sizeof(s_pcm_buf));
        if (ret != ESP_CODEC_DEV_OK) {
            read_errs++;
            consec_errs++;
            if (consec_errs >= REC_MAX_CONSEC_READ_ERRS) {
                ack.err = ESP_FAIL;
                snprintf(ack.message, sizeof(ack.message),
                         "read failed %d times, aborted (last codec err %d)",
                         (int)REC_MAX_CONSEC_READ_ERRS, ret);
                ESP_LOGE(TAG, "%s", ack.message);
                aborted = true;
                break;
            }
            continue; // 该块丢弃，不写假数据
        }
        consec_errs = 0;

        downmix_stereo_to_mono((int16_t *)s_pcm_buf,
                               (int)(sizeof(s_pcm_buf) / 2u / sizeof(int16_t)));
        const uint32_t mono_bytes = (uint32_t)sizeof(s_pcm_buf) / 2u;
        wret = buzz_wav_write(&wav, s_pcm_buf, mono_bytes);
        if (wret != BUZZ_OK) {
            write_errs++;
            ack.err = ESP_FAIL;
            snprintf(ack.message, sizeof(ack.message), "write failed (wav err %d)",
                     wret);
            ESP_LOGE(TAG, "%s", ack.message);
            aborted = true;
            break;
        }
    }

    wret = buzz_wav_close(&wav);
    if (wret != BUZZ_OK) {
        ack.err = ESP_FAIL;
        snprintf(ack.message, sizeof(ack.message), "close failed (wav err %d)", wret);
        ESP_LOGE(TAG, "%s", ack.message);
    }
    s_state = BUZZ_REC_STATE_IDLE;

    const uint64_t data_bytes = wav.data_bytes;
    const uint64_t duration_ms = data_bytes * 1000u / BUZZ_WAV_BYTE_RATE;
    ESP_LOGI(TAG, "recording %s: %s bytes=%" PRIu64 " dur=%" PRIu64 "ms read_errs=%"
             PRIu32 " write_errs=%" PRIu32, aborted ? "aborted" : "done", path,
             data_bytes, duration_ms, read_errs, write_errs);

    if (ack.err == ESP_OK) {
        // 成功应答只回路径（详细统计已在上方日志）；避免长路径 + 统计串越界截断
        snprintf(ack.message, sizeof(ack.message), "%s", path);
    }
    xQueueSend(s_ack_q, &ack, 0);
}

// ---------- 采集任务 ----------

static void rec_task_main(void *arg)
{
    (void)arg;
    for (;;) {
        rec_cmd_t cmd;
        xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY);
        if (cmd == REC_CMD_START) {
            rec_session_run();
        }
        // 空闲态收到的 stop 直接丢弃（REPL 已先行查状态给出提示）
    }
}

// ---------- 公共 API ----------

esp_err_t buzz_recorder_init(esp_codec_dev_handle_t mic)
{
    if (mic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_mic = mic;
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
    s_next_index = scan_max_rec_index() + 1u;
    ESP_LOGI(TAG, "ready, next index %04" PRIu32, s_next_index);
    return ESP_OK;
}

esp_err_t buzz_recorder_start(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == BUZZ_REC_STATE_RECORDING) {
        return ESP_ERR_INVALID_STATE;
    }
    // 清掉可能残留的过期应答（如上次会话读失败中止时无人收取的收尾应答），
    // 避免把旧 ack 误当本次 start 的开工应答
    xQueueReset(s_ack_q);
    rec_cmd_t cmd = REC_CMD_START;
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
    if (s_task == NULL || s_state != BUZZ_REC_STATE_RECORDING) {
        return ESP_ERR_INVALID_STATE;
    }
    rec_cmd_t cmd = REC_CMD_STOP;
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

buzz_rec_state_t buzz_recorder_state(void)
{
    return s_state;
}
