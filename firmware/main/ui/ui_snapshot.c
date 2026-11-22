#include "ui_snapshot.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "ui_task.h"

static const char *TAG = "ui_snap";
#define SNAP_PATH BSP_SD_MOUNT_POINT "/snap.bmp"
#define UI_DEBUG_CODE_SNAPSHOT 1

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void bmp_build_header(uint8_t out[54], uint32_t w, uint32_t h, uint32_t image_size)
{
    memset(out, 0, 54u);
    out[0] = 'B';
    out[1] = 'M';
    wr32(out + 2, 54u + image_size);
    wr32(out + 10, 54u);
    wr32(out + 14, 40u);
    wr32(out + 18, w);
    wr32(out + 22, h);
    wr16(out + 26, 1u);
    wr16(out + 28, 24u);
    wr32(out + 34, image_size);
}

static esp_err_t capture_screen(lv_draw_buf_t *shot, uint8_t **buf_out, uint32_t *w, uint32_t *h)
{
    *buf_out = NULL;
    if (!bsp_display_lock(2000)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_t *scr = lv_screen_active();
    *w = lv_obj_get_width(scr);
    *h = lv_obj_get_height(scr);
    const uint32_t buf_size = *w * *h * 2u;
    uint8_t *buf = heap_caps_malloc(buf_size + 64u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_result_t res = LV_RESULT_INVALID;
    if (buf != NULL) {
        if (lv_draw_buf_init(shot, *w, *h, LV_COLOR_FORMAT_RGB565, 0, buf,
                             buf_size + 64u) == LV_RESULT_OK) {
            res = lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, shot);
        }
    }
    bsp_display_unlock();
    if (buf == NULL || res != LV_RESULT_OK) {
        heap_caps_free(buf);
        ESP_LOGE(TAG, "snapshot failed (buf=%p res=%d)", buf, (int)res);
        return ESP_FAIL;
    }
    *buf_out = buf;
    return ESP_OK;
}

typedef struct {
    FILE *file;
    bool console;
    uint8_t carry[57];
    size_t carry_len;
} bmp_sink_t;

static void console_line(const uint8_t *raw, size_t len)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char encoded[76];
    size_t out = 0;
    for (size_t i = 0; i < len; i += 3u) {
        uint32_t value = (uint32_t)raw[i] << 16;
        const size_t remain = len - i;
        if (remain > 1u) {
            value |= (uint32_t)raw[i + 1u] << 8;
        }
        if (remain > 2u) {
            value |= raw[i + 2u];
        }
        encoded[out++] = table[(value >> 18) & 0x3Fu];
        encoded[out++] = table[(value >> 12) & 0x3Fu];
        encoded[out++] = remain > 1u ? table[(value >> 6) & 0x3Fu] : '=';
        encoded[out++] = remain > 2u ? table[value & 0x3Fu] : '=';
    }
    fwrite(encoded, 1, out, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(1));
}

static bool sink_write(bmp_sink_t *sink, const uint8_t *data, size_t len)
{
    if (!sink->console) {
        return fwrite(data, 1, len, sink->file) == len;
    }
    while (len > 0u) {
        size_t take = sizeof(sink->carry) - sink->carry_len;
        if (take > len) {
            take = len;
        }
        memcpy(sink->carry + sink->carry_len, data, take);
        sink->carry_len += take;
        data += take;
        len -= take;
        if (sink->carry_len == sizeof(sink->carry)) {
            console_line(sink->carry, sink->carry_len);
            sink->carry_len = 0;
        }
    }
    return true;
}

static lv_draw_buf_t s_saved_shot;
static uint8_t *s_saved_buf;
static uint32_t s_saved_w;
static uint32_t s_saved_h;
static bool s_capture_only;

static esp_err_t export_snapshot(bool console)
{
    FILE *file = NULL;
    if (!console) {
        file = fopen(SNAP_PATH, "wb");
        if (file == NULL) {
            ESP_LOGE(TAG, "open %s failed (SD not mounted?)", SNAP_PATH);
            return ESP_ERR_NOT_FOUND;
        }
    }

    lv_draw_buf_t shot;
    uint8_t *buf = NULL;
    uint32_t w = 0;
    uint32_t h = 0;
    esp_err_t ret = ESP_OK;
    if (console && s_saved_buf != NULL) {
        shot = s_saved_shot;
        buf = s_saved_buf;
        w = s_saved_w;
        h = s_saved_h;
        s_saved_buf = NULL;
    } else {
        ret = capture_screen(&shot, &buf, &w, &h);
    }
    if (ret != ESP_OK) {
        if (file != NULL) {
            fclose(file);
        }
        return ret;
    }
    if (console && s_capture_only) {
        s_saved_shot = shot;
        s_saved_buf = buf;
        s_saved_w = w;
        s_saved_h = h;
        return ESP_OK;
    }

    const uint32_t row_raw = w * 3u;
    const uint32_t pad = (4u - (row_raw & 3u)) & 3u;
    const uint32_t row_out = row_raw + pad;
    static uint8_t *row;
    if (row == NULL) {
        row = heap_caps_malloc(row_out, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (row == NULL) {
        heap_caps_free(buf);
        if (file != NULL) {
            fclose(file);
        }
        return ESP_ERR_NO_MEM;
    }

    const uint32_t total_size = 54u + row_out * h;
    bmp_sink_t sink = {.file = file, .console = console, .carry_len = 0};
    if (console) {
        printf("-----BEGIN SNAPUSB size=%" PRIu32 "-----\n", total_size);
    }
    uint8_t header[54];
    bmp_build_header(header, w, h, row_out * h);
    bool ok = sink_write(&sink, header, sizeof(header));

    const uint8_t *fb = (const uint8_t *)shot.data;
    const uint32_t stride = shot.header.stride;
    for (int32_t y = (int32_t)h - 1; ok && y >= 0; y--) {
        const uint16_t *src = (const uint16_t *)(fb + (uint32_t)y * stride);
        for (uint32_t x = 0; x < w; x++) {
            const uint16_t v = src[x];
            row[x * 3u + 0u] = (uint8_t)((v & 0x1Fu) << 3 | (v & 0x1Fu) >> 2);
            row[x * 3u + 1u] = (uint8_t)(((v >> 5) & 0x3Fu) << 2 | ((v >> 5) & 0x3Fu) >> 4);
            row[x * 3u + 2u] = (uint8_t)((v >> 11) << 3 | (v >> 11) >> 2);
        }
        memset(row + row_raw, 0, pad);
        ok = sink_write(&sink, row, row_out);
    }
    heap_caps_free(buf);

    if (console) {
        if (sink.carry_len > 0u) {
            console_line(sink.carry, sink.carry_len);
        }
        fflush(stdout);
        printf("-----END SNAPUSB sent=%" PRIu32 "-----\n", total_size);
        fflush(stdout);
    } else {
        fclose(file);
    }
    if (!ok) {
        ESP_LOGE(TAG, "snapshot write failed");
        return ESP_FAIL;
    }
    if (!console) {
        ESP_LOGI(TAG, "snapshot -> %s (%" PRIu32 "x%" PRIu32 ")", SNAP_PATH, w, h);
        printf("snap saved: %s (%" PRIu32 "x%" PRIu32 ")\n", SNAP_PATH, w, h);
    }
    return ESP_OK;
}

esp_err_t ui_snapshot_to_sdcard(void)
{
    return export_snapshot(false);
}

esp_err_t ui_snapshot_to_console(void)
{
    s_capture_only = true;
    const esp_err_t ret = export_snapshot(true);
    s_capture_only = false;
    return ret;
}

esp_err_t ui_snapshot_console_emit(void)
{
    return export_snapshot(true);
}

static SemaphoreHandle_t s_done;
static esp_err_t s_async_result = ESP_OK;
static bool s_console_request;

void ui_snapshot_execute_from_ui_task(void)
{
    s_async_result = s_console_request ? ui_snapshot_to_console() : ui_snapshot_to_sdcard();
    if (s_done != NULL) {
        xSemaphoreGive(s_done);
    }
}

static esp_err_t request_async(bool console)
{
    if (s_done == NULL) {
        s_done = xSemaphoreCreateBinary();
        if (s_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_console_request = console;
    const ui_msg_t msg = {.src = UI_MSG_SRC_DEBUG, .code = UI_DEBUG_CODE_SNAPSHOT, .value = 0};
    return ui_msg_post(&msg);
}

esp_err_t ui_snapshot_request_async(void)
{
    return request_async(false);
}

esp_err_t ui_snapshot_console_request_async(void)
{
    return request_async(true);
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
