#include "rec_meta.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static uint16_t meta_suffix(const rec_meta_t *meta, uint32_t collision)
{
    uint16_t value = 0;
    for (size_t i = 0; i < 4u; i++) {
        value = (uint16_t)((value << 4) | (uint16_t)hex_nibble(meta->recording_id[i]));
    }
    return (uint16_t)(value + (uint16_t)collision);
}

bool rec_meta_id_valid(const char *recording_id)
{
    if (recording_id == NULL || strlen(recording_id) != REC_META_ID_LEN) {
        return false;
    }
    for (size_t i = 0; i < REC_META_ID_LEN; i++) {
        if (hex_nibble(recording_id[i]) < 0) {
            return false;
        }
    }
    return true;
}

int rec_meta_create(rec_meta_t *meta, const uint8_t random16[16])
{
    if (meta == NULL || random16 == NULL) {
        return -1;
    }
    memset(meta, 0, sizeof(*meta));
    uint8_t uuid[16];
    memcpy(uuid, random16, sizeof(uuid));
    uuid[6] = (uint8_t)((uuid[6] & 0x0Fu) | 0x40u);
    uuid[8] = (uint8_t)((uuid[8] & 0x3Fu) | 0x80u);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(uuid); i++) {
        meta->recording_id[i * 2u] = hex[uuid[i] >> 4];
        meta->recording_id[i * 2u + 1u] = hex[uuid[i] & 0x0Fu];
    }
    meta->recording_id[REC_META_ID_LEN] = '\0';
    return 0;
}

void rec_meta_capture_time(rec_meta_t *meta, const rtc_datetime_t *local, bool rtc_valid,
                           int utc_offset_min, bool time_synced)
{
    if (meta == NULL) {
        return;
    }
    meta->started_at_epoch_ms = 0;
    meta->started_at_utc_offset_min = 0;
    meta->has_started_at = false;
    if (!rtc_valid || local == NULL || !rtc_datetime_valid(local)) {
        meta->time_quality = REC_META_TIME_UNKNOWN;
        return;
    }
    meta->time_quality = REC_META_TIME_UNSYNCED;
    if (!time_synced || utc_offset_min < RTC_OFFSET_MIN_M || utc_offset_min > RTC_OFFSET_MAX_M) {
        return;
    }
    const int64_t local_s = rtc_datetime_to_epoch(local);
    const int64_t utc_s = local_s - (int64_t)utc_offset_min * 60LL;
    if (utc_s < RTC_EPOCH_MIN_S || utc_s > RTC_EPOCH_MAX_S) {
        return;
    }
    meta->started_at_epoch_ms = utc_s * 1000LL;
    meta->started_at_utc_offset_min = utc_offset_min;
    meta->time_quality = REC_META_TIME_SYNCED;
    meta->has_started_at = true;
}

int rec_meta_audio_name(const rec_meta_t *meta, const rtc_datetime_t *local, bool rtc_valid,
                        const char *ext, uint32_t collision, char *out, size_t out_cap)
{
    if (meta == NULL || out == NULL || ext == NULL || !rec_meta_id_valid(meta->recording_id) ||
        (strcasecmp(ext, ".OGG") != 0 && strcasecmp(ext, ".WAV") != 0)) {
        return -1;
    }
    char upper_ext[5];
    for (size_t i = 0; i < 4u; i++) {
        upper_ext[i] = (char)toupper((unsigned char)ext[i]);
    }
    upper_ext[4] = '\0';
    int n;
    if (rtc_valid && local != NULL && rtc_datetime_valid(local)) {
        n = snprintf(out, out_cap, "REC_%04u%02u%02u_%02u%02u%02u_%04X%s",
                     (unsigned)local->year, (unsigned)local->month, (unsigned)local->day,
                     (unsigned)local->hour, (unsigned)local->minute, (unsigned)local->second,
                     (unsigned)meta_suffix(meta, collision), upper_ext);
    } else {
        n = snprintf(out, out_cap, "REC_%.16s_%04X%s", meta->recording_id,
                     (unsigned)meta_suffix(meta, collision), upper_ext);
    }
    return n >= 4 && n <= (int)REC_META_NAME_MAX && (size_t)n < out_cap ? 0 : -1;
}

int rec_meta_sidecar_name(const char *audio_name, char *out, size_t out_cap)
{
    if (audio_name == NULL || out == NULL || strchr(audio_name, '/') != NULL ||
        strchr(audio_name, '\\') != NULL) {
        return -1;
    }
    const char *dot = strrchr(audio_name, '.');
    if (dot == NULL || dot == audio_name) {
        return -1;
    }
    const size_t stem = (size_t)(dot - audio_name);
    const int n = snprintf(out, out_cap, "%.*s.json", (int)stem, audio_name);
    return n > 0 && n <= (int)REC_META_NAME_MAX && (size_t)n < out_cap ? 0 : -1;
}

const char *rec_meta_time_quality_name(rec_meta_time_quality_t quality)
{
    switch (quality) {
    case REC_META_TIME_SYNCED:
        return "synced";
    case REC_META_TIME_UNSYNCED:
        return "unsynced";
    default:
        return "unknown";
    }
}

int rec_meta_encode(const rec_meta_t *meta, char *out, size_t out_cap)
{
    if (meta == NULL || out == NULL || !rec_meta_id_valid(meta->recording_id) ||
        (meta->has_started_at && meta->time_quality != REC_META_TIME_SYNCED)) {
        return -1;
    }
    int n;
    if (meta->has_started_at) {
        n = snprintf(out, out_cap,
                     "{\"schema\":\"%s\",\"recordingID\":\"%s\","
                     "\"startedAtEpochMs\":%" PRId64 ",\"startedAtUtcOffsetMin\":%" PRId32
                     ",\"timeQuality\":\"%s\"}",
                     REC_META_SCHEMA, meta->recording_id, meta->started_at_epoch_ms,
                     meta->started_at_utc_offset_min,
                     rec_meta_time_quality_name(meta->time_quality));
    } else {
        n = snprintf(out, out_cap,
                     "{\"schema\":\"%s\",\"recordingID\":\"%s\",\"timeQuality\":\"%s\"}",
                     REC_META_SCHEMA, meta->recording_id,
                     rec_meta_time_quality_name(meta->time_quality));
    }
    return n > 0 && (size_t)n < out_cap ? n : -1;
}

static const char *json_value(const char *json, const char *key)
{
    char pattern[48];
    const int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) {
        return NULL;
    }
    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        p += (size_t)n;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == ':') {
            p++;
            while (isspace((unsigned char)*p)) {
                p++;
            }
            return p;
        }
    }
    return NULL;
}

static bool json_string(const char *json, const char *key, char *out, size_t out_cap)
{
    const char *p = json_value(json, key);
    if (p == NULL || *p != '"') {
        return false;
    }
    p++;
    size_t n = 0;
    while (p[n] != '\0' && p[n] != '"') {
        if (p[n] == '\\' || n + 1u >= out_cap) {
            return false;
        }
        n++;
    }
    if (p[n] != '"') {
        return false;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static bool json_i64(const char *json, const char *key, int64_t *out)
{
    const char *p = json_value(json, key);
    if (p == NULL) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const long long value = strtoll(p, &end, 10);
    if (end == p || errno == ERANGE) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != ',' && *end != '}') {
        return false;
    }
    *out = (int64_t)value;
    return true;
}

int rec_meta_decode(const char *json, rec_meta_t *out)
{
    if (json == NULL || out == NULL) {
        return -1;
    }
    rec_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    char value[64];
    if (!json_string(json, "schema", value, sizeof(value)) ||
        strcmp(value, REC_META_SCHEMA) != 0 ||
        !json_string(json, "recordingID", meta.recording_id, sizeof(meta.recording_id)) ||
        !rec_meta_id_valid(meta.recording_id) ||
        !json_string(json, "timeQuality", value, sizeof(value))) {
        return -1;
    }
    for (size_t i = 0; i < REC_META_ID_LEN; i++) {
        meta.recording_id[i] = (char)tolower((unsigned char)meta.recording_id[i]);
    }
    if (strcmp(value, "synced") == 0) {
        meta.time_quality = REC_META_TIME_SYNCED;
    } else if (strcmp(value, "unsynced") == 0) {
        meta.time_quality = REC_META_TIME_UNSYNCED;
    } else if (strcmp(value, "unknown") == 0) {
        meta.time_quality = REC_META_TIME_UNKNOWN;
    } else {
        return -1;
    }
    int64_t epoch_ms = 0;
    int64_t offset = 0;
    const bool epoch_present = json_value(json, "startedAtEpochMs") != NULL;
    const bool offset_present = json_value(json, "startedAtUtcOffsetMin") != NULL;
    if (epoch_present != offset_present) {
        return -1;
    }
    if (epoch_present && (!json_i64(json, "startedAtEpochMs", &epoch_ms) ||
                          !json_i64(json, "startedAtUtcOffsetMin", &offset))) {
        return -1;
    }
    if (epoch_present) {
        if (meta.time_quality != REC_META_TIME_SYNCED ||
            epoch_ms < RTC_EPOCH_MIN_S * 1000LL || epoch_ms > RTC_EPOCH_MAX_S * 1000LL + 999LL ||
            offset < RTC_OFFSET_MIN_M || offset > RTC_OFFSET_MAX_M) {
            return -1;
        }
        meta.started_at_epoch_ms = epoch_ms;
        meta.started_at_utc_offset_min = (int32_t)offset;
        meta.has_started_at = true;
    } else if (meta.time_quality == REC_META_TIME_SYNCED) {
        return -1;
    }
    *out = meta;
    return 0;
}

uint32_t rec_meta_ogg_serial(const rec_meta_t *meta)
{
    if (meta == NULL || !rec_meta_id_valid(meta->recording_id)) {
        return 0;
    }
    uint32_t serial = 0;
    for (size_t i = 0; i < 8u; i++) {
        serial = (serial << 4) | (uint32_t)hex_nibble(meta->recording_id[i]);
    }
    return serial;
}
