#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_MODEL_MAX_FILES 128u
#define DEVICE_FILE_ID_CAP 33u

typedef enum {
    DEVICE_STORAGE_UNKNOWN = 0,
    DEVICE_STORAGE_MOUNTED,
    DEVICE_STORAGE_UNAVAILABLE,
    DEVICE_STORAGE_ERROR,
    DEVICE_STORAGE_FORMAT_CANDIDATE,
    DEVICE_STORAGE_FORMAT_CONFIRM,
    DEVICE_STORAGE_FORMATTING,
    DEVICE_STORAGE_FORMAT_FAILED,
} device_storage_state_t;

typedef enum {
    DEVICE_TRANSFER_IDLE = 0,
    DEVICE_TRANSFER_SENDING,
    DEVICE_TRANSFER_WAIT_END,
} device_transfer_state_t;

typedef enum {
    DEVICE_NOTICE_NONE = 0,
    DEVICE_NOTICE_SETTING_SAVE_FAILED,
    DEVICE_NOTICE_FORMAT_DONE,
    DEVICE_NOTICE_FORMAT_SAVE_FAILED,
    DEVICE_NOTICE_FORMAT_FAILED,
    DEVICE_NOTICE_BUSY,
} device_notice_t;

typedef enum {
    DEVICE_FORMAT_EVT_REQUEST = 0,
    DEVICE_FORMAT_EVT_CANCEL,
    DEVICE_FORMAT_EVT_CONFIRM,
    DEVICE_FORMAT_EVT_SUCCEEDED,
    DEVICE_FORMAT_EVT_FAILED,
} device_format_event_t;

typedef struct {
    char id[DEVICE_FILE_ID_CAP];
    uint32_t size;
    uint32_t duration_ms;
    bool synced;
} device_file_t;

typedef struct {
    uint32_t generation;
    device_storage_state_t storage_state;
    int32_t storage_error;
    bool refreshing;
    uint64_t storage_total;
    uint64_t storage_free;
    uint16_t file_count;
    uint16_t total_count;
    uint16_t pending_count;
    device_file_t files[DEVICE_MODEL_MAX_FILES];
    bool ble_connected;
    bool ble_subscribed;
    device_transfer_state_t transfer_state;
    char transfer_id[DEVICE_FILE_ID_CAP];
    uint32_t sent_chunks;
    uint32_t total_chunks;
    bool time_set_valid;
    uint16_t time_year;
    uint8_t time_month;
    uint8_t time_day;
    uint8_t time_hour;
    uint8_t time_minute;
    uint8_t rec_quality;
    uint8_t beep_enabled;
    uint8_t screen_off_s;
    uint32_t notice_seq;
    device_notice_t notice;
} device_snapshot_t;

void device_model_init(void);

// 仅后台唯一生产者使用；返回的内部写缓冲不得传给 LVGL。
device_snapshot_t *device_model_write_begin(void);
void device_model_write_commit(void);
void device_model_publish(const device_snapshot_t *snapshot);

// LVGL 通过值拷贝取得一致性快照，不持有内部缓冲指针。
void device_model_snapshot(device_snapshot_t *out);

void device_model_recount(device_snapshot_t *snapshot);
bool device_model_file_is_active(bool recording_active, const char *active_path,
                                 const char *file_id);
device_storage_state_t device_format_transition(device_storage_state_t state,
                                                 device_format_event_t event,
                                                 bool busy);

#ifdef __cplusplus
}
#endif
