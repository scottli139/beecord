#include "device_model.h"

#include <stdatomic.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define DEVICE_MODEL_EXT_RAM EXT_RAM_BSS_ATTR
#else
#define DEVICE_MODEL_EXT_RAM
#endif

static DEVICE_MODEL_EXT_RAM device_snapshot_t s_snapshot;
static atomic_uint s_sequence;

void device_model_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    atomic_init(&s_sequence, 0u);
}

device_snapshot_t *device_model_write_begin(void)
{
    atomic_fetch_add_explicit(&s_sequence, 1u, memory_order_acq_rel);
    return &s_snapshot;
}

void device_model_write_commit(void)
{
    s_snapshot.generation++;
    atomic_fetch_add_explicit(&s_sequence, 1u, memory_order_release);
}

void device_model_publish(const device_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    (void)device_model_write_begin();
    const uint32_t generation = s_snapshot.generation;
    memcpy(&s_snapshot, snapshot, sizeof(s_snapshot));
    s_snapshot.generation = generation;
    device_model_write_commit();
}

void device_model_snapshot(device_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    for (;;) {
        const unsigned before = atomic_load_explicit(&s_sequence, memory_order_acquire);
        if ((before & 1u) != 0u) {
            continue;
        }
        memcpy(out, &s_snapshot, sizeof(*out));
        const unsigned after = atomic_load_explicit(&s_sequence, memory_order_acquire);
        if (before == after) {
            return;
        }
    }
}

void device_model_recount(device_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    uint32_t pending = 0;
    for (uint32_t i = 0; i < snapshot->file_count && i < DEVICE_MODEL_MAX_FILES; i++) {
        if (!snapshot->files[i].synced) {
            pending++;
        }
    }
    snapshot->pending_count = (uint16_t)pending;
}

bool device_model_file_is_active(bool recording_active, const char *active_path,
                                 const char *file_id)
{
    if (!recording_active || active_path == NULL || file_id == NULL) {
        return false;
    }
    const char *base = strrchr(active_path, '/');
    base = base == NULL ? active_path : base + 1;
    return base[0] != '\0' && strcmp(base, file_id) == 0;
}

device_storage_state_t device_format_transition(device_storage_state_t state,
                                                 device_format_event_t event,
                                                 bool busy)
{
    switch (event) {
    case DEVICE_FORMAT_EVT_REQUEST:
        if (!busy && (state == DEVICE_STORAGE_FORMAT_CANDIDATE ||
                      state == DEVICE_STORAGE_FORMAT_FAILED)) {
            return DEVICE_STORAGE_FORMAT_CONFIRM;
        }
        return state;
    case DEVICE_FORMAT_EVT_CANCEL:
        if (state == DEVICE_STORAGE_FORMAT_CONFIRM || state == DEVICE_STORAGE_FORMAT_FAILED) {
            return DEVICE_STORAGE_FORMAT_CANDIDATE;
        }
        return state;
    case DEVICE_FORMAT_EVT_CONFIRM:
        return !busy && state == DEVICE_STORAGE_FORMAT_CONFIRM ? DEVICE_STORAGE_FORMATTING : state;
    case DEVICE_FORMAT_EVT_SUCCEEDED:
        return state == DEVICE_STORAGE_FORMATTING ? DEVICE_STORAGE_MOUNTED : state;
    case DEVICE_FORMAT_EVT_FAILED:
        return state == DEVICE_STORAGE_FORMATTING ? DEVICE_STORAGE_FORMAT_FAILED : state;
    default:
        return state;
    }
}
