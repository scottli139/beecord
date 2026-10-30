// Beecord 已同步标记集合实现（sync_marks.c，M5.4）。
#include "sync_marks.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// 持久化 blob 头：count(u16 LE) + 每项 u8 len + 字节
#define BLOB_HDR 2u

static char s_names[SYNC_MARKS_MAX][SYNC_MARKS_NAME_MAX + 1u];
static uint32_t s_count;
static const sync_marks_store_t *s_store;
static void *s_ctx;
static bool s_save_ok = true;

// 编码 scratch：约 8.45KB 必须静态——调用方 buzz_cmds 任务栈仅 4KB，栈分配必溢出
//（M5.4 复查实锤）。**单上下文约定**：add/init 不得并发调用（设备端 buzz_cmds
// 任务串行下发 DL_END、init 在启动期早于一切任务，天然满足；宿主单测同线程）。
static uint8_t s_blob_buf[BLOB_HDR + SYNC_MARKS_MAX * (1u + SYNC_MARKS_NAME_MAX)];

// 返回 0=已持久化（或无持久化义务） -2=保存失败（运行态已更新，持久态停在上次成功版）
static int persist(void)
{
    s_save_ok = true;
    if (s_store == NULL || s_store->save == NULL) {
        return 0;
    }
    uint8_t *buf = s_blob_buf;
    uint32_t o = 0;
    buf[o++] = (uint8_t)(s_count & 0xFFu);
    buf[o++] = (uint8_t)(s_count >> 8);
    for (uint32_t i = 0; i < s_count; i++) {
        const uint8_t len = (uint8_t)strlen(s_names[i]);
        buf[o++] = len;
        memcpy(&buf[o], s_names[i], len);
        o += len;
    }
    if (s_store->save(s_ctx, buf, o) < 0) {
        s_save_ok = false; // 运行态完整；持久态停在上次成功版本
        return -2;
    }
    return 0;
}

static int find_index(const char *name)
{
    for (uint32_t i = 0; i < s_count; i++) {
        if (strcmp(s_names[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// 把第 idx 项移到最新位（LRU 刷新）
static void move_to_newest(uint32_t idx)
{
    char tmp[SYNC_MARKS_NAME_MAX + 1u];
    snprintf(tmp, sizeof(tmp), "%s", s_names[idx]);
    memmove(&s_names[idx], &s_names[idx + 1u],
            (s_count - idx - 1u) * sizeof(s_names[0]));
    snprintf(s_names[s_count - 1u], sizeof(s_names[0]), "%s", tmp);
}

void sync_marks_init(const sync_marks_store_t *store, void *ctx)
{
    s_store = store;
    s_ctx = ctx;
    s_count = 0;
    s_save_ok = true;
    if (store == NULL || store->load == NULL) {
        return;
    }
    uint8_t *buf = s_blob_buf; // 静态 scratch：与 persist 共用（串行约定见上）
    const int n = store->load(ctx, buf, sizeof(s_blob_buf));
    if (n < (int)BLOB_HDR) {
        return; // 无持久态
    }
    const uint32_t count = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8);
    if (count > SYNC_MARKS_MAX) {
        return; // 坏 blob：视为空（不回写，保留现场）
    }
    uint32_t o = BLOB_HDR;
    for (uint32_t i = 0; i < count; i++) {
        if (o >= (uint32_t)n) {
            s_count = 0; // 截断=损坏
            return;
        }
        const uint8_t len = buf[o++];
        if (len > SYNC_MARKS_NAME_MAX || o + len > (uint32_t)n || len == 0u) {
            s_count = 0; // 损坏
            return;
        }
        memcpy(s_names[i], &buf[o], len);
        s_names[i][len] = '\0';
        o += len;
    }
    s_count = count;
}

int sync_marks_add(const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) > SYNC_MARKS_NAME_MAX) {
        return -1;
    }
    const int existing = find_index(name);
    if (existing >= 0) {
        move_to_newest((uint32_t)existing); // 幂等：重复下载刷新为最新
        return persist() < 0 ? -2 : 1; // 刷新成功=已存在；保存失败=-2
    }
    if (s_count < SYNC_MARKS_MAX) {
        snprintf(s_names[s_count], sizeof(s_names[0]), "%s", name);
        s_count++;
    } else {
        // 满：淘汰最久未用（[0]），新项落最新位
        memmove(&s_names[0], &s_names[1],
                (SYNC_MARKS_MAX - 1u) * sizeof(s_names[0]));
        snprintf(s_names[SYNC_MARKS_MAX - 1u], sizeof(s_names[0]), "%s", name);
    }
    return persist() < 0 ? -2 : 0;
}

bool sync_marks_has(const char *name)
{
    if (name == NULL) {
        return false;
    }
    return find_index(name) >= 0;
}

uint32_t sync_marks_count(void)
{
    return s_count;
}

uint32_t sync_marks_capacity(void)
{
    return SYNC_MARKS_MAX;
}

const char *sync_marks_at(uint32_t idx)
{
    if (idx >= s_count) {
        return NULL;
    }
    return s_names[idx];
}

bool sync_marks_last_save_ok(void)
{
    return s_save_ok;
}
