#include <stdint.h>
#include <string.h>

#include "rec_meta.h"
#include "test.h"

static const rtc_datetime_t k_local = {
    .year = 2026, .month = 11, .day = 22, .hour = 19, .minute = 12, .second = 21,
};

static rec_meta_t sample_meta(void)
{
    uint8_t random[16];
    for (uint8_t i = 0; i < sizeof(random); i++) {
        random[i] = i;
    }
    rec_meta_t meta;
    CHECK_EQ_INT(rec_meta_create(&meta, random), 0);
    return meta;
}

static void test_uuid_v4(void)
{
    const rec_meta_t meta = sample_meta();
    CHECK(strcmp(meta.recording_id, "000102030405460788090a0b0c0d0e0f") == 0);
    CHECK(rec_meta_id_valid(meta.recording_id));
    CHECK_EQ_INT(meta.recording_id[12], '4');
    CHECK_EQ_INT(meta.recording_id[16], '8');
    CHECK_EQ_INT((int)rec_meta_ogg_serial(&meta), 0x00010203);
    CHECK(!rec_meta_id_valid("abc"));
    CHECK(!rec_meta_id_valid("000102030405460788890a0b0c0d0e0g"));
}

static void test_capture_synced_time(void)
{
    rec_meta_t meta = sample_meta();
    rec_meta_capture_time(&meta, &k_local, true, 480, true);
    CHECK(meta.has_started_at);
    CHECK_EQ_INT(meta.time_quality, REC_META_TIME_SYNCED);
    CHECK(meta.started_at_epoch_ms == 1795345941000LL);
    CHECK_EQ_INT(meta.started_at_utc_offset_min, 480);
}

static void test_capture_degrades_without_rtc_or_sync(void)
{
    rec_meta_t meta = sample_meta();
    rec_meta_capture_time(&meta, &k_local, true, 0, false);
    CHECK(!meta.has_started_at);
    CHECK_EQ_INT(meta.time_quality, REC_META_TIME_UNSYNCED);
    rec_meta_capture_time(&meta, &k_local, false, 480, true);
    CHECK_EQ_INT(meta.time_quality, REC_META_TIME_UNKNOWN);
    rtc_datetime_t invalid = k_local;
    invalid.month = 13;
    rec_meta_capture_time(&meta, &invalid, true, 480, true);
    CHECK_EQ_INT(meta.time_quality, REC_META_TIME_UNKNOWN);
}

static void test_audio_and_sidecar_names(void)
{
    rec_meta_t meta = sample_meta();
    char name[REC_META_NAME_MAX + 1u];
    CHECK_EQ_INT(rec_meta_audio_name(&meta, &k_local, true, ".OGG", 0,
                                    name, sizeof(name)), 0);
    CHECK(strcmp(name, "REC_20261122_191221_0001.OGG") == 0);
    CHECK_EQ_INT(rec_meta_audio_name(&meta, &k_local, true, ".wav", 1,
                                    name, sizeof(name)), 0);
    CHECK(strcmp(name, "REC_20261122_191221_0002.WAV") == 0);
    char sidecar[REC_META_NAME_MAX + 1u];
    CHECK_EQ_INT(rec_meta_sidecar_name(name, sidecar, sizeof(sidecar)), 0);
    CHECK(strcmp(sidecar, "REC_20261122_191221_0002.json") == 0);
    CHECK_EQ_INT(rec_meta_audio_name(&meta, &k_local, false, ".OGG", 0,
                                    name, sizeof(name)), 0);
    CHECK(strcmp(name, "REC_0001020304054607_0001.OGG") == 0);
    CHECK_EQ_INT(strlen(name), 29u);
}

static void test_name_rejects_bad_input(void)
{
    rec_meta_t meta = sample_meta();
    char name[REC_META_NAME_MAX + 1u];
    CHECK_EQ_INT(rec_meta_audio_name(&meta, &k_local, true, ".MP3", 0,
                                    name, sizeof(name)), -1);
    CHECK_EQ_INT(rec_meta_audio_name(&meta, &k_local, true, ".OGG", 0, name, 4), -1);
    CHECK_EQ_INT(rec_meta_sidecar_name("x/y.OGG", name, sizeof(name)), -1);
    CHECK_EQ_INT(rec_meta_sidecar_name("NOEXT", name, sizeof(name)), -1);
}

static void test_synced_json_roundtrip(void)
{
    rec_meta_t meta = sample_meta();
    rec_meta_capture_time(&meta, &k_local, true, 480, true);
    char json[REC_META_JSON_CAP];
    CHECK(rec_meta_encode(&meta, json, sizeof(json)) > 0);
    CHECK(strstr(json, "beecord-recording/1") != NULL);
    rec_meta_t decoded;
    CHECK_EQ_INT(rec_meta_decode(json, &decoded), 0);
    CHECK(strcmp(decoded.recording_id, meta.recording_id) == 0);
    CHECK(decoded.has_started_at);
    CHECK(decoded.started_at_epoch_ms == meta.started_at_epoch_ms);
    CHECK_EQ_INT(decoded.started_at_utc_offset_min, 480);
    CHECK_EQ_INT(decoded.time_quality, REC_META_TIME_SYNCED);
}

static void test_unsynced_json_roundtrip(void)
{
    rec_meta_t meta = sample_meta();
    rec_meta_capture_time(&meta, &k_local, true, 0, false);
    char json[REC_META_JSON_CAP];
    CHECK(rec_meta_encode(&meta, json, sizeof(json)) > 0);
    CHECK(strstr(json, "startedAtEpochMs") == NULL);
    rec_meta_t decoded;
    CHECK_EQ_INT(rec_meta_decode(json, &decoded), 0);
    CHECK(!decoded.has_started_at);
    CHECK_EQ_INT(decoded.time_quality, REC_META_TIME_UNSYNCED);
}

static void test_decode_normalizes_uppercase_id(void)
{
    const char *json = "{\"schema\":\"beecord-recording/1\","
                       "\"recordingID\":\"000102030405460788890A0B0C0D0E0F\","
                       "\"timeQuality\":\"unknown\",\"futureField\":123}";
    rec_meta_t decoded;
    CHECK_EQ_INT(rec_meta_decode(json, &decoded), 0);
    CHECK(strcmp(decoded.recording_id, "000102030405460788890a0b0c0d0e0f") == 0);
}

static void test_decode_rejects_bad_schema_and_time_pairs(void)
{
    rec_meta_t decoded;
    CHECK_EQ_INT(rec_meta_decode("{}", &decoded), -1);
    CHECK_EQ_INT(rec_meta_decode(
                     "{\"schema\":\"other/1\",\"recordingID\":"
                     "\"000102030405460788890a0b0c0d0e0f\",\"timeQuality\":\"unknown\"}",
                     &decoded), -1);
    CHECK_EQ_INT(rec_meta_decode(
                     "{\"schema\":\"beecord-recording/1\",\"recordingID\":"
                     "\"000102030405460788890a0b0c0d0e0f\",\"timeQuality\":\"synced\","
                     "\"startedAtEpochMs\":1795345941000}",
                     &decoded), -1);
    CHECK_EQ_INT(rec_meta_decode(
                     "{\"schema\":\"beecord-recording/1\",\"recordingID\":"
                     "\"000102030405460788890a0b0c0d0e0f\",\"timeQuality\":\"synced\"}",
                     &decoded), -1);
    CHECK_EQ_INT(rec_meta_decode(
                     "{\"schema\":\"beecord-recording/1\",\"recordingID\":"
                     "\"000102030405460788890a0b0c0d0e0f\",\"timeQuality\":\"unknown\","
                     "\"startedAtEpochMs\":\"bad\",\"startedAtUtcOffsetMin\":480}",
                     &decoded), -1);
}

int main(void)
{
    RUN_TEST(test_uuid_v4);
    RUN_TEST(test_capture_synced_time);
    RUN_TEST(test_capture_degrades_without_rtc_or_sync);
    RUN_TEST(test_audio_and_sidecar_names);
    RUN_TEST(test_name_rejects_bad_input);
    RUN_TEST(test_synced_json_roundtrip);
    RUN_TEST(test_unsynced_json_roundtrip);
    RUN_TEST(test_decode_normalizes_uppercase_id);
    RUN_TEST(test_decode_rejects_bad_schema_and_time_pairs);
    return test_report();
}
