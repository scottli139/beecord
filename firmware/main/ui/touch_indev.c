// 原生 LVGL 触摸 indev 实现（touch_indev.c，M5.2 缺陷修复）。
#include "touch_indev.h"

#include "esp_log.h"

static const char *TAG = "touch";

#define TOUCH_ERR_LOG_EVERY 64u // 错误计数观测：每 64 次失败打一条警告（防刷屏）

typedef struct {
    esp_lcd_touch_handle_t tp;
    int32_t last_x;
    int32_t last_y;
    lv_indev_state_t last_state;
    uint32_t read_total;
    uint32_t read_err;
} touch_indev_ctx_t;

static touch_indev_ctx_t s_ctx;

// LVGL indev 读回调（LVGL 任务上下文，period 由 indev 驱动轮询）
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    touch_indev_ctx_t *ctx = &s_ctx;
    ctx->read_total++;

    // 读失败不 abort（esp_lvgl_port 胶层会）：保留上次坐标/状态，
    // 错误计数 + 每 N 次警告日志。FT5x06 无触点 NACK / 总线竞争偶发
    // ESP_ERR_INVALID_STATE（M5.2 实机实锤），下一轮自恢复。
    esp_err_t r = esp_lcd_touch_read_data(ctx->tp);
    if (r == ESP_OK) {
        esp_lcd_touch_point_data_t pt[1] = {0};
        uint8_t cnt = 0;
        r = esp_lcd_touch_get_data(ctx->tp, pt, &cnt, 1);
        if (r == ESP_OK && cnt > 0) {
            ctx->last_x = pt[0].x;
            ctx->last_y = pt[0].y;
            ctx->last_state = LV_INDEV_STATE_PRESSED;
        } else {
            // 读成功但无触点（或取点失败）：按释放处理
            ctx->last_state = LV_INDEV_STATE_RELEASED;
        }
    } else {
        ctx->read_err++;
        if (ctx->read_err % TOUCH_ERR_LOG_EVERY == 1) {
            ESP_LOGW(TAG, "touch read err %u/%u (%s)，保留上次坐标",
                     (unsigned)ctx->read_err, (unsigned)ctx->read_total,
                     esp_err_to_name(r));
        }
    }

    data->point.x = (int16_t)ctx->last_x;
    data->point.y = (int16_t)ctx->last_y;
    data->state = ctx->last_state;
}

lv_indev_t *ui_touch_indev_create(lv_display_t *disp, esp_lcd_touch_handle_t tp)
{
    if (disp == NULL || tp == NULL) {
        return NULL;
    }
    s_ctx.tp = tp;
    s_ctx.last_x = 0;
    s_ctx.last_y = 0;
    s_ctx.last_state = LV_INDEV_STATE_RELEASED;
    s_ctx.read_total = 0;
    s_ctx.read_err = 0;

    lv_indev_t *indev = lv_indev_create();
    if (indev == NULL) {
        return NULL;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);
    return indev;
}

void ui_touch_indev_stats(uint32_t *out_total, uint32_t *out_err)
{
    if (out_total != NULL) {
        *out_total = s_ctx.read_total;
    }
    if (out_err != NULL) {
        *out_err = s_ctx.read_err;
    }
}
