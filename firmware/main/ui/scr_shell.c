#include "scr_shell.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "buzz_ble.h"
#include "buzz_cmds.h"
#include "fw_version.h"
#include "theme.h"
#include "ui_toast.h"

static lv_obj_t *s_list_box;
static lv_obj_t *s_list_status;
static uint32_t s_list_signature = UINT32_MAX;
static device_storage_state_t s_list_storage = DEVICE_STORAGE_UNKNOWN;

#define LIST_ROW_STRIDE 64
#define LIST_VISIBLE_ROWS 8

typedef struct {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *sub;
    lv_obj_t *sync;
} list_row_widgets_t;

static list_row_widgets_t s_rows[LIST_VISIBLE_ROWS];
static const device_snapshot_t *s_view_snapshot;
static uint16_t s_view_count;
static uint16_t s_first_visible = UINT16_MAX;

static lv_obj_t *s_sync_state;
static lv_obj_t *s_sync_subscription;
static lv_obj_t *s_sync_pending;
static lv_obj_t *s_sync_file;
static lv_obj_t *s_sync_percent;
static lv_obj_t *s_sync_bar;
static lv_obj_t *s_sync_time;

static lv_obj_t *s_setting_values[5];
static lv_obj_t *s_setting_rows[5];

void scr_format_duration(uint32_t duration_ms, char *out, size_t cap)
{
    const uint32_t seconds = duration_ms / 1000u;
    const uint32_t hours = seconds / 3600u;
    const uint32_t minutes = (seconds / 60u) % 60u;
    const uint32_t secs = seconds % 60u;
    if (hours > 0u) {
        snprintf(out, cap, "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32, hours, minutes, secs);
    } else {
        snprintf(out, cap, "%02" PRIu32 ":%02" PRIu32, minutes, secs);
    }
}

void scr_format_size(uint32_t size, char *out, size_t cap)
{
    if (size >= 1024u * 1024u) {
        snprintf(out, cap, "%.1f MB", (double)size / (1024.0 * 1024.0));
    } else if (size >= 1024u) {
        snprintf(out, cap, "%" PRIu32 " KB", (size + 1023u) / 1024u);
    } else {
        snprintf(out, cap, "%" PRIu32 " B", size);
    }
}

lv_obj_t *scr_page_begin(lv_obj_t *tile)
{
    lv_obj_set_style_bg_opa(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_scrollable(tile, false);

    lv_obj_t *page = lv_obj_create(tile);
    lv_obj_set_size(page, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_opa(page, 0, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_hor(page, UI_SAFE_HOR, 0);
    lv_obj_set_style_pad_top(page, UI_SAFE_TOP, 0);
    lv_obj_set_style_pad_bottom(page, UI_PAGE_PAD_BOTTOM, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(page, false);
    return page;
}

void scr_page_title(lv_obj_t *page, const char *title)
{
    lv_obj_t *lab = lv_label_create(page);
    lv_obj_set_width(lab, LV_PCT(100));
    lv_label_set_text(lab, title);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
    ui_theme_apply_text(lab, FONT_TITLE, UI_COLOR_TEXT_PRIMARY);
}

static lv_obj_t *make_card(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), height);
    lv_obj_set_style_bg_color(card, UI_COLOR_CARD, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_LG, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, UI_SPACE_3, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, UI_SPACE_2, 0);
    lv_obj_set_scrollable(card, false);
    return card;
}

static lv_obj_t *make_text(lv_obj_t *parent, const char *text, const lv_font_t *font,
                           lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    ui_theme_apply_text(label, font, color);
    return label;
}

static uint32_t list_signature(const device_snapshot_t *snapshot)
{
    uint32_t hash = 2166136261u;
    for (uint16_t i = 0; i < snapshot->file_count; i++) {
        const device_file_t *file = &snapshot->files[i];
        for (size_t j = 0; j < sizeof(file->id); j++) {
            hash = (hash ^ (uint8_t)file->id[j]) * 16777619u;
        }
        hash = (hash ^ file->size) * 16777619u;
        hash = (hash ^ file->duration_ms) * 16777619u;
        hash = (hash ^ (uint32_t)file->synced) * 16777619u;
    }
    hash ^= snapshot->file_count | ((uint32_t)snapshot->pending_count << 16);
    return hash;
}

static void list_row_create(lv_obj_t *parent, uint8_t slot)
{
    list_row_widgets_t *widgets = &s_rows[slot];
    widgets->row = lv_obj_create(parent);
    lv_obj_set_size(widgets->row, LV_PCT(100), 56);
    lv_obj_set_style_bg_color(widgets->row, UI_COLOR_CARD, 0);
    lv_obj_set_style_radius(widgets->row, 12, 0);
    lv_obj_set_style_border_width(widgets->row, 0, 0);
    lv_obj_set_style_pad_all(widgets->row, 0, 0);
    lv_obj_set_scrollable(widgets->row, false);
    lv_obj_set_clickable(widgets->row, false);

    const int32_t text_width = UI_SCREEN_W - 2 * UI_SAFE_HOR - 2 * UI_SPACE_3 -
                               UI_SPACE_6 - UI_SPACE_2;
    widgets->name = make_text(widgets->row, "", FONT_BODY, UI_COLOR_TEXT_PRIMARY);
    lv_obj_set_width(widgets->name, text_width);
    lv_label_set_long_mode(widgets->name, LV_LABEL_LONG_DOT);
    lv_obj_align(widgets->name, LV_ALIGN_TOP_LEFT, UI_SPACE_3, UI_SPACE_1);

    widgets->sub = make_text(widgets->row, "", FONT_CAPTION, UI_COLOR_TEXT_SECONDARY);
    lv_obj_set_width(widgets->sub, text_width);
    lv_label_set_long_mode(widgets->sub, LV_LABEL_LONG_DOT);
    lv_obj_align(widgets->sub, LV_ALIGN_BOTTOM_LEFT, UI_SPACE_3, -UI_SPACE_1);

    widgets->sync = make_text(widgets->row, "", FONT_BODY, UI_COLOR_SUCCESS);
    lv_obj_set_width(widgets->sync, UI_SPACE_6);
    lv_obj_set_style_text_align(widgets->sync, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(widgets->sync, LV_ALIGN_RIGHT_MID, -UI_SPACE_3, 0);
    lv_obj_set_hidden(widgets->row, true);
}

static void list_visible_update(void)
{
    int32_t scroll_y = lv_obj_get_scroll_y(s_list_box);
    if (scroll_y < 0) {
        scroll_y = 0;
    }
    uint16_t first = (uint16_t)(scroll_y / LIST_ROW_STRIDE);
    if (first >= s_view_count) {
        first = s_view_count > 0u ? s_view_count - 1u : 0u;
    }
    if (first == s_first_visible) {
        return;
    }
    s_first_visible = first;
    for (uint16_t slot = 0; slot < LIST_VISIBLE_ROWS; slot++) {
        const uint16_t index = first + slot;
        list_row_widgets_t *widgets = &s_rows[slot];
        if (index >= s_view_count) {
            lv_obj_set_hidden(widgets->row, true);
            continue;
        }
        const device_file_t *file = &s_view_snapshot->files[index];
        lv_label_set_text(widgets->name, file->id);
        char duration[16];
        char size[20];
        char meta[40];
        scr_format_duration(file->duration_ms, duration, sizeof(duration));
        scr_format_size(file->size, size, sizeof(size));
        snprintf(meta, sizeof(meta), "%s · %s", duration, size);
        lv_label_set_text(widgets->sub, meta);
        lv_label_set_text(widgets->sync, file->synced ? "✓" : "");
        lv_obj_set_pos(widgets->row, 0, (int32_t)index * LIST_ROW_STRIDE);
        lv_obj_set_hidden(widgets->row, false);
    }
}

static void list_scrolled(lv_event_t *event)
{
    (void)event;
    if (s_view_count > 0u) {
        list_visible_update();
    }
}

static void list_populate(const device_snapshot_t *snapshot)
{
    s_view_count = snapshot->file_count;
    s_view_snapshot = snapshot;
    s_first_visible = UINT16_MAX;
    lv_obj_clean(s_list_box);
    lv_obj_set_style_layout(s_list_box, LV_LAYOUT_NONE, 0);
    lv_obj_scroll_to_y(s_list_box, 0, LV_ANIM_OFF);

    lv_obj_t *anchor = lv_obj_create(s_list_box);
    lv_obj_set_size(anchor, LV_PCT(100), s_view_count * LIST_ROW_STRIDE - UI_SPACE_2);
    lv_obj_set_pos(anchor, 0, 0);
    lv_obj_set_style_bg_opa(anchor, 0, 0);
    lv_obj_set_style_border_width(anchor, 0, 0);
    lv_obj_set_style_pad_all(anchor, 0, 0);
    lv_obj_set_scrollable(anchor, false);
    lv_obj_set_clickable(anchor, false);
    for (uint8_t slot = 0; slot < LIST_VISIBLE_ROWS; slot++) {
        list_row_create(anchor, slot);
    }
    list_visible_update();
}

void scr_list_create(lv_obj_t *tile)
{
    lv_obj_t *page = scr_page_begin(tile);
    scr_page_title(page, "录音列表");
    s_list_status = make_text(page, "", FONT_CAPTION, UI_COLOR_TEXT_SECONDARY);
    lv_obj_set_width(s_list_status, LV_PCT(100));
    lv_obj_set_style_text_align(s_list_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(s_list_status, UI_SPACE_1, 0);

    s_list_box = lv_obj_create(page);
    lv_obj_set_size(s_list_box, LV_PCT(100), 0);
    lv_obj_set_flex_grow(s_list_box, 1);
    lv_obj_set_style_bg_opa(s_list_box, 0, 0);
    lv_obj_set_style_border_width(s_list_box, 0, 0);
    lv_obj_set_style_pad_all(s_list_box, 0, 0);
    lv_obj_set_style_pad_row(s_list_box, UI_SPACE_2, 0);
    lv_obj_set_style_margin_top(s_list_box, UI_SPACE_2, 0);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_chain_hor(s_list_box, true);
    lv_obj_add_event_cb(s_list_box, list_scrolled, LV_EVENT_SCROLL, NULL);
}

static void list_empty(const char *line1, const char *line2)
{
    s_view_count = 0;
    s_view_snapshot = NULL;
    s_first_visible = UINT16_MAX;
    lv_obj_clean(s_list_box);
    lv_obj_set_flex_align(s_list_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t *box = lv_obj_create(s_list_box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, 0, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_row(box, UI_SPACE_2, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(box, false);
    make_text(box, line1, FONT_BODY, UI_COLOR_TEXT_PRIMARY);
    make_text(box, line2, FONT_CAPTION, UI_COLOR_TEXT_SECONDARY);
}

void scr_list_render(const device_snapshot_t *snapshot)
{
    const uint32_t signature = list_signature(snapshot);
    lv_label_set_text(s_list_status, snapshot->refreshing ? "正在刷新…" : "");
    if (signature == s_list_signature && snapshot->storage_state == s_list_storage) {
        return;
    }
    s_list_signature = signature;
    s_list_storage = snapshot->storage_state;

    if (snapshot->storage_state == DEVICE_STORAGE_UNAVAILABLE) {
        list_empty("无存储卡", "存储不可用");
        return;
    }
    if (snapshot->storage_state == DEVICE_STORAGE_FORMAT_CANDIDATE ||
        snapshot->storage_state == DEVICE_STORAGE_FORMAT_CONFIRM ||
        snapshot->storage_state == DEVICE_STORAGE_FORMAT_FAILED) {
        list_empty("无法识别存储卡", "请确认是否格式化");
        return;
    }
    if (snapshot->storage_state == DEVICE_STORAGE_FORMATTING) {
        list_empty("格式化中…", "请稍候");
        return;
    }
    if (snapshot->storage_state == DEVICE_STORAGE_ERROR) {
        list_empty("存储读取失败", "请重新进入本页重试");
        return;
    }
    if (snapshot->storage_state == DEVICE_STORAGE_UNKNOWN) {
        list_empty("正在读取存储…", "请稍候");
        return;
    }
    if (snapshot->file_count == 0u) {
        list_empty("暂无录音", "按功能键录一条");
        return;
    }

    list_populate(snapshot);
}

void scr_list_debug_scroll(int32_t y)
{
    if (s_list_box == NULL) {
        return;
    }
    lv_obj_scroll_to_y(s_list_box, y, LV_ANIM_OFF);
    list_visible_update();
}

void scr_sync_create(lv_obj_t *tile)
{
    lv_obj_t *page = scr_page_begin(tile);
    lv_obj_set_style_pad_row(page, UI_SPACE_3, 0);
    scr_page_title(page, "同步");

    lv_obj_t *ble = make_card(page, 96);
    make_text(ble, "BLE 状态", FONT_CAPTION, UI_COLOR_TEXT_SECONDARY);
    s_sync_state = make_text(ble, "广播中…", FONT_BODY, UI_COLOR_TEXT_PRIMARY);
    s_sync_subscription = make_text(ble, "等待 Mac 连接", FONT_CAPTION,
                                    UI_COLOR_TEXT_SECONDARY);

    lv_obj_t *files = make_card(page, 148);
    s_sync_pending = make_text(files, "待同步 -- 条", FONT_BODY, UI_COLOR_TEXT_PRIMARY);
    s_sync_file = make_text(files, "等待 Mac 发起同步", FONT_CAPTION,
                            UI_COLOR_TEXT_SECONDARY);
    lv_obj_set_width(s_sync_file, LV_PCT(100));
    lv_label_set_long_mode(s_sync_file, LV_LABEL_LONG_DOT);
    s_sync_bar = lv_bar_create(files);
    lv_obj_set_size(s_sync_bar, LV_PCT(100), 6);
    lv_bar_set_range(s_sync_bar, 0, 100);
    lv_obj_set_style_bg_color(s_sync_bar, UI_COLOR_CARD2, 0);
    lv_obj_set_style_bg_color(s_sync_bar, UI_COLOR_SYNC, LV_PART_INDICATOR);
    s_sync_percent = make_text(files, "当前文件 --%", FONT_CAPTION,
                               UI_COLOR_TEXT_SECONDARY);

    lv_obj_t *time = make_card(page, 76);
    make_text(time, "对时状态", FONT_CAPTION, UI_COLOR_TEXT_SECONDARY);
    s_sync_time = make_text(time, "本次启动未对时", FONT_BODY, UI_COLOR_TEXT_PRIMARY);
}

void scr_sync_render(const device_snapshot_t *snapshot)
{
    lv_label_set_text(s_sync_state, snapshot->ble_connected ? "已连接 Studio" : "广播中…");
    lv_obj_set_style_text_color(s_sync_state,
                                snapshot->ble_connected ? UI_COLOR_SYNC : UI_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(s_sync_subscription,
                      !snapshot->ble_connected ? "等待 Mac 连接"
                      : snapshot->ble_subscribed ? "通知已订阅" : "等待通知订阅");

    char buf[64];
    if (snapshot->storage_state == DEVICE_STORAGE_MOUNTED) {
        snprintf(buf, sizeof(buf), "待同步 %u 条", (unsigned)snapshot->pending_count);
    } else {
        snprintf(buf, sizeof(buf), "待同步 -- 条");
    }
    lv_label_set_text(s_sync_pending, buf);

    int32_t progress = 0;
    if (snapshot->transfer_state == DEVICE_TRANSFER_IDLE) {
        lv_label_set_text(s_sync_file, "等待 Mac 发起同步");
        lv_label_set_text(s_sync_percent, "当前文件 --%");
    } else {
        lv_label_set_text(s_sync_file, snapshot->transfer_id);
        if (snapshot->total_chunks > 0u) {
            progress = (int32_t)((uint64_t)snapshot->sent_chunks * 100u /
                                 snapshot->total_chunks);
        } else if (snapshot->transfer_state == DEVICE_TRANSFER_WAIT_END) {
            progress = 100;
        }
        snprintf(buf, sizeof(buf), "%s %" PRId32 "%%",
                 snapshot->transfer_state == DEVICE_TRANSFER_WAIT_END ? "等待完成确认"
                                                                       : "当前文件",
                 progress);
        lv_label_set_text(s_sync_percent, buf);
    }
    lv_bar_set_value(s_sync_bar, progress, LV_ANIM_OFF);

    if (snapshot->time_set_valid) {
        snprintf(buf, sizeof(buf), "自动 · %02u-%02u %02u:%02u",
                 (unsigned)snapshot->time_month, (unsigned)snapshot->time_day,
                 (unsigned)snapshot->time_hour, (unsigned)snapshot->time_minute);
        lv_label_set_text(s_sync_time, buf);
    } else {
        lv_label_set_text(s_sync_time, "本次启动未对时");
    }
}

static void setting_clicked(lv_event_t *event)
{
    const buzz_local_setting_t setting = (buzz_local_setting_t)(intptr_t)lv_event_get_user_data(event);
    if (buzz_cmds_cycle_setting(setting) != ESP_OK) {
        ui_toast_show("操作繁忙，请重试", true);
    }
}

static void setting_row(lv_obj_t *page, uint16_t index, const char *name, bool clickable,
                        buzz_local_setting_t setting)
{
    lv_obj_t *row = lv_obj_create(page);
    s_setting_rows[index] = row;
    lv_obj_set_size(row, LV_PCT(100), UI_TOUCH_MIN);
    lv_obj_set_style_bg_color(row, UI_COLOR_CARD, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, UI_SPACE_3, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_SPACE_2, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, false);
    if (clickable) {
        lv_obj_set_clickable(row, true);
        lv_obj_add_event_cb(row, setting_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)setting);
    }
    make_text(row, name, FONT_BODY, UI_COLOR_TEXT_PRIMARY);
    s_setting_values[index] = make_text(row, "", FONT_CAPTION, UI_COLOR_TEXT_SECONDARY);
    lv_obj_set_width(s_setting_values[index], 220);
    lv_label_set_long_mode(s_setting_values[index], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_setting_values[index], LV_TEXT_ALIGN_RIGHT, 0);
}

void scr_settings_create(lv_obj_t *tile)
{
    lv_obj_t *page = scr_page_begin(tile);
    lv_obj_set_style_pad_row(page, UI_SPACE_2, 0);
    scr_page_title(page, "设置");
    lv_obj_set_style_margin_bottom(lv_obj_get_child(page, 0), UI_SPACE_1, 0);
    setting_row(page, 0, "熄屏时间", true, BUZZ_LOCAL_SETTING_SCREEN_OFF);
    setting_row(page, 1, "录音质量", true, BUZZ_LOCAL_SETTING_QUALITY);
    setting_row(page, 2, "提示音", true, BUZZ_LOCAL_SETTING_BEEP);
    setting_row(page, 3, "对时状态", false, 0);
    setting_row(page, 4, "关于", false, 0);
    lv_obj_t *hint = make_text(page, "质量下次生效 · 下滑返回", FONT_CAPTION,
                               UI_COLOR_TEXT_SECONDARY);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

void scr_settings_debug_click(uint16_t index)
{
    if (index < 3u && s_setting_rows[index] != NULL) {
        lv_obj_send_event(s_setting_rows[index], LV_EVENT_CLICKED, NULL);
    }
}

void scr_settings_render(const device_snapshot_t *snapshot)
{
    char buf[64];
    if (snapshot->screen_off_s == 0u) {
        lv_label_set_text(s_setting_values[0], "永不 ›");
    } else {
        snprintf(buf, sizeof(buf), "%us ›", (unsigned)snapshot->screen_off_s);
        lv_label_set_text(s_setting_values[0], buf);
    }
    lv_label_set_text(s_setting_values[1],
                      snapshot->rec_quality == 0u ? "标准 44kbps ›" : "省空间 24kbps ›");
    lv_label_set_text(s_setting_values[2], snapshot->beep_enabled ? "开 ›" : "关 ›");
    if (snapshot->time_set_valid) {
        snprintf(buf, sizeof(buf), "自动 · %02u-%02u %02u:%02u",
                 (unsigned)snapshot->time_month, (unsigned)snapshot->time_day,
                 (unsigned)snapshot->time_hour, (unsigned)snapshot->time_minute);
    } else {
        snprintf(buf, sizeof(buf), "本次启动未对时");
    }
    lv_label_set_text(s_setting_values[3], buf);
    snprintf(buf, sizeof(buf), "%s · %s", FW_VERSION, buzz_ble_device_name());
    lv_label_set_text(s_setting_values[4], buf);
}
