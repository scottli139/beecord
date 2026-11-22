#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_model.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *scr_page_begin(lv_obj_t *tile);
void scr_page_title(lv_obj_t *page, const char *title);

void scr_format_duration(uint32_t duration_ms, char *out, size_t cap);
void scr_format_size(uint32_t size, char *out, size_t cap);

void scr_list_create(lv_obj_t *tile);
void scr_sync_create(lv_obj_t *tile);
void scr_settings_create(lv_obj_t *tile);

void scr_list_render(const device_snapshot_t *snapshot);
void scr_sync_render(const device_snapshot_t *snapshot);
void scr_settings_render(const device_snapshot_t *snapshot);
void scr_list_debug_scroll(int32_t y);
void scr_settings_debug_click(uint16_t index);

#ifdef __cplusplus
}
#endif
