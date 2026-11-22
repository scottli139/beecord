#include <string.h>

#include "../main/ui/device_model.h"
#include "test.h"

static void test_snapshot_generation_and_values(void)
{
    device_model_init();
    device_snapshot_t out;
    device_model_snapshot(&out);
    CHECK_EQ_INT(out.generation, 0u);
    CHECK_EQ_INT(out.storage_state, DEVICE_STORAGE_UNKNOWN);

    device_snapshot_t *w = device_model_write_begin();
    w->storage_state = DEVICE_STORAGE_MOUNTED;
    w->storage_total = 1000u;
    w->storage_free = 250u;
    device_model_write_commit();
    device_model_snapshot(&out);
    CHECK_EQ_INT(out.generation, 1u);
    CHECK_EQ_INT(out.storage_state, DEVICE_STORAGE_MOUNTED);
    CHECK_EQ_INT(out.storage_total, 1000u);
    CHECK_EQ_INT(out.storage_free, 250u);
}

static void test_pending_count_and_active_file_exclusion_contract(void)
{
    device_model_init();
    device_snapshot_t *w = device_model_write_begin();
    w->file_count = 3;
    w->total_count = 3;
    strcpy(w->files[0].id, "REC_0003.OGG");
    w->files[0].synced = false;
    strcpy(w->files[1].id, "REC_0002.OGG");
    w->files[1].synced = true;
    strcpy(w->files[2].id, "REC_0001.WAV");
    w->files[2].synced = false;
    device_model_recount(w);
    device_model_write_commit();

    device_snapshot_t out;
    device_model_snapshot(&out);
    CHECK_EQ_INT(out.pending_count, 2u);
    CHECK(strcmp(out.files[0].id, "REC_0003.OGG") == 0);
    w = device_model_write_begin();
    memmove(&w->files[0], &w->files[1], 2u * sizeof(w->files[0]));
    w->file_count = 2;
    w->total_count = 2;
    device_model_recount(w);
    device_model_write_commit();
    device_model_snapshot(&out);
    CHECK_EQ_INT(out.file_count, 2u);
    CHECK_EQ_INT(out.pending_count, 1u);
    CHECK(strcmp(out.files[0].id, "REC_0002.OGG") == 0);
}

static void test_format_cancel_and_success(void)
{
    device_storage_state_t s = DEVICE_STORAGE_FORMAT_CANDIDATE;
    s = device_format_transition(s, DEVICE_FORMAT_EVT_REQUEST, false);
    CHECK_EQ_INT(s, DEVICE_STORAGE_FORMAT_CONFIRM);
    s = device_format_transition(s, DEVICE_FORMAT_EVT_CANCEL, false);
    CHECK_EQ_INT(s, DEVICE_STORAGE_FORMAT_CANDIDATE);
    s = device_format_transition(s, DEVICE_FORMAT_EVT_REQUEST, false);
    s = device_format_transition(s, DEVICE_FORMAT_EVT_CONFIRM, false);
    CHECK_EQ_INT(s, DEVICE_STORAGE_FORMATTING);
    s = device_format_transition(s, DEVICE_FORMAT_EVT_SUCCEEDED, false);
    CHECK_EQ_INT(s, DEVICE_STORAGE_MOUNTED);
}

static void test_format_busy_repeat_and_failure(void)
{
    CHECK_EQ_INT(device_format_transition(DEVICE_STORAGE_FORMAT_CANDIDATE,
                                          DEVICE_FORMAT_EVT_REQUEST, true),
                 DEVICE_STORAGE_FORMAT_CANDIDATE);
    device_storage_state_t s = device_format_transition(DEVICE_STORAGE_FORMAT_CANDIDATE,
                                                        DEVICE_FORMAT_EVT_REQUEST, false);
    CHECK_EQ_INT(device_format_transition(s, DEVICE_FORMAT_EVT_CONFIRM, true), s);
    s = device_format_transition(s, DEVICE_FORMAT_EVT_CONFIRM, false);
    CHECK_EQ_INT(device_format_transition(s, DEVICE_FORMAT_EVT_CONFIRM, false), s);
    CHECK_EQ_INT(device_format_transition(s, DEVICE_FORMAT_EVT_CANCEL, false), s);
    s = device_format_transition(s, DEVICE_FORMAT_EVT_FAILED, false);
    CHECK_EQ_INT(s, DEVICE_STORAGE_FORMAT_FAILED);
    s = device_format_transition(s, DEVICE_FORMAT_EVT_REQUEST, false);
    CHECK_EQ_INT(s, DEVICE_STORAGE_FORMAT_CONFIRM);
}

static void test_format_invalid_completion_rejected(void)
{
    CHECK_EQ_INT(device_format_transition(DEVICE_STORAGE_MOUNTED,
                                          DEVICE_FORMAT_EVT_REQUEST, false),
                 DEVICE_STORAGE_MOUNTED);
    CHECK_EQ_INT(device_format_transition(DEVICE_STORAGE_FORMAT_CANDIDATE,
                                          DEVICE_FORMAT_EVT_SUCCEEDED, false),
                 DEVICE_STORAGE_FORMAT_CANDIDATE);
    CHECK_EQ_INT(device_format_transition(DEVICE_STORAGE_FORMAT_FAILED,
                                          DEVICE_FORMAT_EVT_FAILED, false),
                 DEVICE_STORAGE_FORMAT_FAILED);
}

static void test_publish_leaves_current_snapshot_available_during_build(void)
{
    device_model_init();
    device_snapshot_t *w = device_model_write_begin();
    w->storage_state = DEVICE_STORAGE_MOUNTED;
    w->pending_count = 2;
    device_model_write_commit();

    device_snapshot_t next;
    device_model_snapshot(&next);
    next.pending_count = 7;
    device_snapshot_t current;
    device_model_snapshot(&current);
    CHECK_EQ_INT(current.pending_count, 2u);
    device_model_publish(&next);
    device_model_snapshot(&current);
    CHECK_EQ_INT(current.pending_count, 7u);
    CHECK_EQ_INT(current.generation, 2u);
}

static void test_active_file_requires_active_session(void)
{
    CHECK(device_model_file_is_active(true, "/sdcard/REC_0002.OGG", "REC_0002.OGG"));
    CHECK(!device_model_file_is_active(false, "/sdcard/REC_0002.OGG", "REC_0002.OGG"));
    CHECK(!device_model_file_is_active(true, "/sdcard/REC_0002.OGG", "REC_0001.OGG"));
    CHECK(!device_model_file_is_active(true, "", "REC_0002.OGG"));
    CHECK(!device_model_file_is_active(true, NULL, "REC_0002.OGG"));
}

int main(void)
{
    RUN_TEST(test_snapshot_generation_and_values);
    RUN_TEST(test_pending_count_and_active_file_exclusion_contract);
    RUN_TEST(test_format_cancel_and_success);
    RUN_TEST(test_format_busy_repeat_and_failure);
    RUN_TEST(test_format_invalid_completion_rejected);
    RUN_TEST(test_publish_leaves_current_snapshot_available_during_build);
    RUN_TEST(test_active_file_requires_active_session);
    return test_report();
}
