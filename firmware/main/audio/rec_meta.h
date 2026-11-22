#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REC_META_SCHEMA "beecord-recording/1"
#define REC_META_ID_LEN 32u
#define REC_META_NAME_MAX 32u
#define REC_META_JSON_CAP 384u

typedef enum {
    REC_META_TIME_UNKNOWN = 0,
    REC_META_TIME_UNSYNCED,
    REC_META_TIME_SYNCED,
} rec_meta_time_quality_t;

typedef struct {
    char recording_id[REC_META_ID_LEN + 1u];
    int64_t started_at_epoch_ms;
    int32_t started_at_utc_offset_min;
    rec_meta_time_quality_t time_quality;
    bool has_started_at;
} rec_meta_t;

// 以 16 个随机字节初始化 UUIDv4；输入为空或输出为空返回 -1。
int rec_meta_create(rec_meta_t *meta, const uint8_t random16[16]);
bool rec_meta_id_valid(const char *recording_id);

// 冻结起录时间。RTC 有效且已有对时上下文时才产生 UTC 时刻与原始偏移。
void rec_meta_capture_time(rec_meta_t *meta, const rtc_datetime_t *local, bool rtc_valid,
                           int utc_offset_min, bool time_synced);

// 生成不覆盖序号语义的浏览文件名。collision 用于同秒同后缀冲突探测。
// ext 仅接受 ".OGG"/".WAV"（大小写均可）。成功返回 0，参数/容量错误返回 -1。
int rec_meta_audio_name(const rec_meta_t *meta, const rtc_datetime_t *local, bool rtc_valid,
                        const char *ext, uint32_t collision, char *out, size_t out_cap);
int rec_meta_sidecar_name(const char *audio_name, char *out, size_t out_cap);

// sidecar JSON 编解码。解码要求 schema/UUID/时间字段组合完整，忽略未知字段。
int rec_meta_encode(const rec_meta_t *meta, char *out, size_t out_cap);
int rec_meta_decode(const char *json, rec_meta_t *out);
const char *rec_meta_time_quality_name(rec_meta_time_quality_t quality);
uint32_t rec_meta_ogg_serial(const rec_meta_t *meta);

#ifdef __cplusplus
}
#endif
