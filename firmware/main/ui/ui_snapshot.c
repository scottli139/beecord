// LVGL 快照 → BMP 实现（ui_snapshot.c，M5.1）。
// RGB565 帧缓冲转 24bit BMP（逐行 BGR、自底向上、4 字节对齐），
// 不整帧转 RGB888——只在行缓冲里做像素换算，峰值内存 = 一行 1232B。
#include "ui_snapshot.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lvgl.h"

#include "ui_task.h"

static const char *TAG = "ui_snap";

#define SNAP_PATH BSP_SD_MOUNT_POINT "/snap.bmp"

// BMP 文件头/信息头（54B，小端逐字段写）
static void bmp_write_header(FILE *f, uint32_t w, uint32_t h, uint32_t img_size)
{
    const uint32_t off_bits = 54;
    const uint32_t file_size = off_bits + img_size;
    const uint32_t zero = 0;
    const uint32_t info_sz = 40;
    const uint16_t planes = 1;
    const uint16_t bpp = 24;
    const int32_t h_s = (int32_t)h; // 正高度 = 自底向上行序

    fwrite("BM", 1, 2, f);
    fwrite(&file_size, 4, 1, f);
    fwrite(&zero, 4, 1, f);
    fwrite(&off_bits, 4, 1, f);
    fwrite(&info_sz, 4, 1, f);
    fwrite(&w, 4, 1, f);
    fwrite(&h_s, 4, 1, f);
    fwrite(&planes, 2, 1, f);
    fwrite(&bpp, 2, 1, f);
    fwrite(&zero, 4, 1, f);  // 无压缩
    fwrite(&img_size, 4, 1, f);
    fwrite(&zero, 4, 1, f);  // X ppm
    fwrite(&zero, 4, 1, f);  // Y ppm
    fwrite(&zero, 4, 1, f);  // 调色板
    fwrite(&zero, 4, 1, f);  // 重要色
}

esp_err_t ui_snapshot_to_sdcard(void)
{
    FILE *f = fopen(SNAP_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open %s failed（TF 卡未挂载？）", SNAP_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    // 快照缓冲 ~412KB（410×502×2B）：SPIRAM_USE_MALLOC=n，malloc 不会自动落 PSRAM，
    // 必须显式 heap_caps 分配并交给 take_to_draw_buf（lv_snapshot_take 内部走 malloc 必失败）。
    if (!bsp_display_lock(2000)) {
        fclose(f);
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_t *scr = lv_screen_active();
    const uint32_t w = lv_obj_get_width(scr);
    const uint32_t h = lv_obj_get_height(scr);

    const uint32_t buf_size = w * h * 2; // RGB565 紧凑行（stride 由 LVGL 自算，可能略大；给 64B 余量）
    uint8_t *buf = heap_caps_malloc(buf_size + 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_draw_buf_t shot;
    lv_result_t res = LV_RESULT_INVALID;
    if (buf != NULL) {
        if (lv_draw_buf_init(&shot, w, h, LV_COLOR_FORMAT_RGB565, 0, buf, buf_size + 64) == LV_RESULT_OK) {
            res = lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, &shot);
        }
    }
    bsp_display_unlock();

    if (buf == NULL || res != LV_RESULT_OK) {
        heap_caps_free(buf);
        fclose(f);
        ESP_LOGE(TAG, "snapshot failed (buf=%p res=%d)", buf, (int)res);
        return ESP_FAIL;
    }

    const uint32_t row_raw = w * 3;
    const uint32_t pad = (4 - (row_raw & 3)) & 3;
    const uint32_t row_out = row_raw + pad;
    bmp_write_header(f, w, h, row_out * h);

    // 行缓冲放 PSRAM（静态避免占任务栈）
    static uint8_t *row = NULL;
    if (row == NULL) {
        row = heap_caps_malloc(row_out, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (row == NULL) {
        heap_caps_free(buf);
        fclose(f);
        ESP_LOGE(TAG, "row buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    const uint8_t *fb = (const uint8_t *)shot.data;
    const uint32_t stride = shot.header.stride; // 字节；≥ w*2
    for (int32_t y = (int32_t)h - 1; y >= 0; y--) {
        const uint16_t *src = (const uint16_t *)(fb + (uint32_t)y * stride);
        for (uint32_t x = 0; x < w; x++) {
            uint16_t v = src[x];
            row[x * 3 + 0] = (uint8_t)((v & 0x1F) << 3 | (v & 0x1F) >> 2);        // B
            row[x * 3 + 1] = (uint8_t)(((v >> 5) & 0x3F) << 2 | ((v >> 5) & 0x3F) >> 4); // G
            row[x * 3 + 2] = (uint8_t)((v >> 11) << 3 | (v >> 11) >> 2);          // R
        }
        memset(row + row_raw, 0, pad);
        if (fwrite(row, 1, row_out, f) != row_out) {
            heap_caps_free(buf);
            fclose(f);
            ESP_LOGE(TAG, "write failed");
            return ESP_FAIL;
        }
    }

    // draw_buf 结构体在栈上、数据缓冲外部持有（非 ALLOCATED 标记），直接 free 数据即可
    heap_caps_free(buf);
    fclose(f);
    ESP_LOGI(TAG, "snapshot → %s (%" PRIu32 "x%" PRIu32 ")", SNAP_PATH, w, h);
    printf("snap saved: %s (%" PRIu32 "x%" PRIu32 ")\n", SNAP_PATH, w, h);
    return ESP_OK;
}

// ---------- 异步派发（LVGL 任务执行，REPL 等结果） ----------

#define UI_DEBUG_CODE_SNAPSHOT 1

static SemaphoreHandle_t s_done; // 二值信号量，懒创建；REPL 单使用者，无并发请求
static esp_err_t s_async_result = ESP_OK;

void ui_snapshot_execute_from_ui_task(void)
{
    s_async_result = ui_snapshot_to_sdcard();
    if (s_done != NULL) {
        xSemaphoreGive(s_done);
    }
}

esp_err_t ui_snapshot_request_async(void)
{
    if (s_done == NULL) {
        s_done = xSemaphoreCreateBinary();
        if (s_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    const ui_msg_t msg = {.src = UI_MSG_SRC_DEBUG, .code = UI_DEBUG_CODE_SNAPSHOT, .value = 0};
    return ui_msg_post(&msg);
}

esp_err_t ui_snapshot_wait_done(uint32_t timeout_ms)
{
    if (s_done == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return s_async_result;
}
