// 录音控制层实现（ui_rec.c，M5.3）。
#include "ui_rec.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "audio/buzz_recorder.h"
#include "storage/buzz_sdcard.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "sys/beep_play.h"
#include "ui_task.h"
#include "ui_toast.h"

static const char *TAG = "ui_rec";

#define UI_REC_TASK_STACK 3072
#define UI_REC_TASK_PRIO 2
#define UI_REC_Q_LEN 8

// 开始录音的剩余空间下限（<10% 总容量拒绝，ux-device §7"存储将满"）
#define UI_REC_MIN_FREE_PCT 10u

typedef enum {
    UI_REC_CMD_TOGGLE = 0,
    UI_REC_CMD_PAUSE_TOGGLE,
    UI_REC_CMD_MARK,
} ui_rec_cmd_t;

typedef struct {
    uint8_t kind; // 0=命令 1=recorder 事件
    uint8_t code;
    int32_t value;
    char message[80];
} ui_rec_qmsg_t;

static QueueHandle_t s_q;
static char s_recovered_file[80];
static bool s_recovered_pending;

// ---------- 事件转发 ----------

static void forward_to_ui(uint8_t code, int32_t value, const char *message)
{
    ui_msg_t msg = {
        .src = UI_MSG_SRC_REC,
        .code = (int32_t)code,
        .value = value,
    };
    (void)message; // 详情走控制任务日志；UI 侧只用 code/value
    ui_msg_post(&msg);
}

// recorder 事件回调（采集任务上下文）：入队给控制任务，严禁在此阻塞/播 beep
static void on_recorder_event(buzz_rec_evt_t evt, const char *message, int32_t value,
                              void *ctx)
{
    (void)ctx;
    if (s_q == NULL) {
        return;
    }
    ui_rec_qmsg_t m = { .kind = 1, .code = (uint8_t)evt, .value = value, .message = { 0 } };
    snprintf(m.message, sizeof(m.message), "%s", message != NULL ? message : "");
    if (xQueueSend(s_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "rec event dropped (queue full): %d", (int)evt);
    }
}

// ---------- 提示音映射（ux-device §8） ----------

static void beep_for_event(uint8_t evt)
{
    switch ((buzz_rec_evt_t)evt) {
    case BUZZ_REC_EVT_STARTED:
        (void)beep_play(BEEP_EVT_REC_START);
        break;
    case BUZZ_REC_EVT_STOPPED:
        (void)beep_play(BEEP_EVT_REC_STOP);
        break;
    case BUZZ_REC_EVT_PAUSED:
        (void)beep_play(BEEP_EVT_PAUSE);
        break;
    case BUZZ_REC_EVT_RESUMED:
        (void)beep_play(BEEP_EVT_RESUME);
        break;
    case BUZZ_REC_EVT_MARKED:
        (void)beep_play(BEEP_EVT_MARK);
        break;
    case BUZZ_REC_EVT_ERROR:
    case BUZZ_REC_EVT_AUTO_STOPPED:
        (void)beep_play(BEEP_EVT_ERROR);
        break;
    default:
        break;
    }
}

// ---------- 开始前置检查 ----------

// 返回 ESP_OK；否则给 toast + 错误音并返回错误码（ux-device §7 存储行为）
static esp_err_t check_can_start(void)
{
    if (!buzz_sdcard_mounted()) {
        ui_toast_show("无存储卡，无法录音", true);
        (void)beep_play(BEEP_EVT_ERROR);
        return ESP_ERR_NOT_FOUND;
    }
    uint64_t total = 0;
    uint64_t free_ = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free_) != ESP_OK) {
        ui_toast_show("存储不可用", true);
        (void)beep_play(BEEP_EVT_ERROR);
        return ESP_FAIL;
    }
    if (total > 0 && free_ * 100u / total < UI_REC_MIN_FREE_PCT) {
        ui_toast_show("存储空间不足", true);
        (void)beep_play(BEEP_EVT_ERROR);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---------- 控制任务 ----------

static void do_toggle(void)
{
    buzz_rec_state_t st = buzz_recorder_state();
    if (st == BUZZ_REC_STATE_IDLE) {
        if (check_can_start() != ESP_OK) {
            forward_to_ui((uint8_t)BUZZ_REC_EVT_ERROR, 0, NULL);
            return;
        }
        esp_err_t ret = buzz_recorder_start(BUZZ_REC_FMT_OGG);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(ret));
            ui_toast_show("录音启动失败", true);
            (void)beep_play(BEEP_EVT_ERROR);
            forward_to_ui((uint8_t)BUZZ_REC_EVT_ERROR, 0, NULL);
        }
        // 成功路径：STARTED 事件已入队，统一由事件分支处理提示音/页面
    } else {
        esp_err_t ret = buzz_recorder_stop();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "stop failed: %s", esp_err_to_name(ret));
            ui_toast_show("停止失败", true);
        }
        // STOPPED 事件统一走事件分支
    }
}

static void do_pause_toggle(void)
{
    buzz_rec_state_t st = buzz_recorder_state();
    esp_err_t ret;
    if (st == BUZZ_REC_STATE_RECORDING) {
        ret = buzz_recorder_pause();
    } else if (st == BUZZ_REC_STATE_PAUSED) {
        ret = buzz_recorder_resume();
    } else {
        return; // 非会话态忽略双击
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "pause/resume failed: %s", esp_err_to_name(ret));
    }
}

static void do_mark(void)
{
    esp_err_t ret = buzz_recorder_mark();
    if (ret == ESP_ERR_NO_MEM) {
        ui_toast_show("标记已满", true);
    }
    // MARKED 事件走事件分支；暂停态 mark 被 recorder 拒绝（无事件，符合预期）
}

static void ui_rec_task(void *arg)
{
    (void)arg;
    ui_rec_qmsg_t m;
    for (;;) {
        xQueueReceive(s_q, &m, portMAX_DELAY);
        if (m.kind == 0) {
            switch ((ui_rec_cmd_t)m.code) {
            case UI_REC_CMD_TOGGLE:
                do_toggle();
                break;
            case UI_REC_CMD_PAUSE_TOGGLE:
                do_pause_toggle();
                break;
            case UI_REC_CMD_MARK:
                do_mark();
                break;
            default:
                break;
            }
            continue;
        }
        // 事件分支：提示音 + 转发 UI
        ESP_LOGI(TAG, "evt=%d value=%d %s", (int)m.code, (int)m.value, m.message);
        beep_for_event(m.code);
        if ((buzz_rec_evt_t)m.code == BUZZ_REC_EVT_AUTO_STOPPED) {
            ui_toast_show("存储已满，已自动保存", true);
        } else if ((buzz_rec_evt_t)m.code == BUZZ_REC_EVT_ERROR) {
            ui_toast_show("录音错误，已停止", true);
        }
        forward_to_ui(m.code, m.value, m.message);
    }
}

// ---------- 公共 API ----------

static esp_err_t post_cmd(ui_rec_cmd_t cmd)
{
    if (s_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ui_rec_qmsg_t m = { .kind = 0, .code = (uint8_t)cmd, .value = 0, .message = { 0 } };
    return xQueueSend(s_q, &m, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t ui_rec_request_toggle(void)
{
    return post_cmd(UI_REC_CMD_TOGGLE);
}

esp_err_t ui_rec_request_pause_toggle(void)
{
    return post_cmd(UI_REC_CMD_PAUSE_TOGGLE);
}

esp_err_t ui_rec_request_mark(void)
{
    return post_cmd(UI_REC_CMD_MARK);
}

void ui_rec_set_recovered(const char *file)
{
    snprintf(s_recovered_file, sizeof(s_recovered_file), "%s",
             file != NULL ? file : "");
    s_recovered_pending = true;
}

bool ui_rec_take_recovered(char *out_file, size_t cap)
{
    if (!s_recovered_pending) {
        return false;
    }
    s_recovered_pending = false;
    if (out_file != NULL && cap > 0) {
        snprintf(out_file, cap, "%s", s_recovered_file);
    }
    return true;
}

esp_err_t ui_rec_init(void)
{
    s_q = xQueueCreate(UI_REC_Q_LEN, sizeof(ui_rec_qmsg_t));
    ESP_RETURN_ON_FALSE(s_q != NULL, ESP_ERR_NO_MEM, TAG, "queue create failed");
    buzz_recorder_set_event_cb(on_recorder_event, NULL);
    BaseType_t ok = xTaskCreate(ui_rec_task, "ui_rec", UI_REC_TASK_STACK, NULL,
                                UI_REC_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    ESP_LOGI(TAG, "rec control up");
    return ESP_OK;
}
