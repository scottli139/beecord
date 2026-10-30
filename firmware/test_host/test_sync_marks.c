// sync_marks 已同步标记宿主单测（M5.4，M5_PLAN D3）：DL_END 标记语义、
// 256 上限 LRU、幂等刷新、非法 id、持久化往返与失败行为。内存 blob 后端。
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../main/sys/sync_marks.h"
#include "test.h"

static uint8_t s_blob[8192];
static int s_blob_len; // -1 = 无持久态
static bool s_fail_save;

static int blob_load(void *ctx, uint8_t *buf, uint32_t cap)
{
    (void)ctx;
    if (s_blob_len < 0) {
        return -1;
    }
    if (cap < (uint32_t)s_blob_len) {
        return -1;
    }
    memcpy(buf, s_blob, (size_t)s_blob_len);
    return s_blob_len;
}

static int blob_save(void *ctx, const uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (s_fail_save) {
        return -1;
    }
    if (len > sizeof(s_blob)) {
        return -1;
    }
    memcpy(s_blob, buf, len);
    s_blob_len = (int)len;
    return 0;
}

static const sync_marks_store_t s_store = { .load = blob_load, .save = blob_save };

static void backend_reset(void)
{
    s_blob_len = -1;
    s_fail_save = false;
    memset(s_blob, 0, sizeof(s_blob));
}

static void make_name(char *out, size_t cap, int i)
{
    snprintf(out, cap, "REC_%04d.OGG", i);
}

static void test_add_has_count(void)
{
    backend_reset();
    sync_marks_init(&s_store, NULL);
    CHECK(sync_marks_add("/REC_0001.OGG") == 0);
    CHECK(sync_marks_add("/REC_0002.OGG") == 0);
    CHECK(sync_marks_has("/REC_0001.OGG"));
    CHECK(!sync_marks_has("/REC_9999.OGG"));
    CHECK(sync_marks_count() == 2u);
}

static void test_duplicate_idempotent_and_refreshes_lru(void)
{
    backend_reset();
    sync_marks_init(&s_store, NULL);
    sync_marks_add("/A.OGG");
    sync_marks_add("/B.OGG");
    sync_marks_add("/C.OGG");
    CHECK(sync_marks_add("/A.OGG") == 1); // 已存在：幂等
    CHECK(sync_marks_count() == 3u);
    // A 刷新为最新 → 淘汰顺序里 A 最后死：at(2)=A
    CHECK(strcmp(sync_marks_at(2), "/A.OGG") == 0);
}

static void test_lru_eviction_order(void)
{
    backend_reset();
    sync_marks_init(&s_store, NULL);
    char name[24];
    for (int i = 0; i < (int)SYNC_MARKS_MAX; i++) {
        make_name(name, sizeof(name), i);
        CHECK(sync_marks_add(name) == 0);
    }
    CHECK(sync_marks_count() == SYNC_MARKS_MAX);
    // 满后再加：淘汰最老的 REC_0000，新项在最新位
    CHECK(sync_marks_add("/NEW.OGG") == 0);
    CHECK(sync_marks_count() == SYNC_MARKS_MAX);
    CHECK(!sync_marks_has("REC_0000.OGG"));
    CHECK(sync_marks_has("REC_0001.OGG"));
    CHECK(sync_marks_has("/NEW.OGG"));
    CHECK(strcmp(sync_marks_at(SYNC_MARKS_MAX - 1u), "/NEW.OGG") == 0);
}

static void test_lru_eviction_respects_refresh(void)
{
    backend_reset();
    sync_marks_init(&s_store, NULL);
    char name[24];
    for (int i = 0; i < (int)SYNC_MARKS_MAX; i++) {
        make_name(name, sizeof(name), i);
        sync_marks_add(name);
    }
    sync_marks_add("REC_0001.OGG"); // 刷新 1 为最新 → 最老变成 REC_0000
    sync_marks_add("/X.OGG");
    CHECK(sync_marks_has("REC_0001.OGG")); // 1 被刷新逃过淘汰
    CHECK(!sync_marks_has("REC_0000.OGG")); // 最老者被淘汰
    CHECK(sync_marks_has("REC_0002.OGG")); // 未刷新者幸存
}

static void test_illegal_names_rejected(void)
{
    backend_reset();
    sync_marks_init(&s_store, NULL);
    CHECK(sync_marks_add(NULL) == -1);
    CHECK(sync_marks_add("") == -1);
    char long_name[SYNC_MARKS_NAME_MAX + 2u];
    memset(long_name, 'a', sizeof(long_name) - 1u);
    long_name[sizeof(long_name) - 1u] = '\0';
    CHECK(sync_marks_add(long_name) == -1);
    CHECK(sync_marks_count() == 0u);
    CHECK(sync_marks_last_save_ok()); // 拒绝路径不置失败
}

static void test_persist_and_reload_order(void)
{
    backend_reset();
    sync_marks_init(&s_store, NULL);
    sync_marks_add("/A.OGG");
    sync_marks_add("/B.OGG");
    sync_marks_add("/C.OGG");
    // 模拟重启
    sync_marks_init(&s_store, NULL);
    CHECK(sync_marks_count() == 3u);
    CHECK(strcmp(sync_marks_at(0), "/A.OGG") == 0); // LRU 顺序保留
    CHECK(sync_marks_has("/B.OGG"));
    CHECK(sync_marks_add("/A.OGG") == 1); // 重下幂等
}

static void test_corrupt_blob_treated_empty(void)
{
    backend_reset();
    s_blob_len = 8;
    memset(s_blob, 0xFF, 8); // count=65535：超上限
    sync_marks_init(&s_store, NULL);
    CHECK(sync_marks_count() == 0u);
    CHECK(sync_marks_add("/A.OGG") == 0); // 仍能正常工作
    CHECK(sync_marks_count() == 1u);
}

static void test_truncated_blob_treated_empty(void)
{
    backend_reset();
    sync_marks_init(&s_store, NULL);
    sync_marks_add("/A.OGG");
    sync_marks_add("/B.OGG");
    const int good_len = s_blob_len;
    s_blob_len = good_len - 2; // 模拟断电撕尾
    sync_marks_init(&s_store, NULL);
    CHECK(sync_marks_count() == 0u);
}

static void test_save_failure_keeps_runtime(void)
{
    backend_reset();
    sync_marks_init(&s_store, NULL);
    sync_marks_add("/A.OGG"); // 先成功
    s_fail_save = true;
    CHECK(sync_marks_add("/B.OGG") == -2); // 运行成功但持久化失败（M5.4 复查修正）
    CHECK(sync_marks_has("/B.OGG"));
    CHECK(!sync_marks_last_save_ok());
    // 重启：持久态=上次成功（只有 A）
    s_fail_save = false;
    sync_marks_init(&s_store, NULL);
    CHECK(sync_marks_count() == 1u);
    CHECK(sync_marks_has("/A.OGG"));
    CHECK(!sync_marks_has("/B.OGG"));
}

static void test_refresh_save_failure_returns_minus2(void)
{
    backend_reset();
    sync_marks_init(&s_store, NULL);
    sync_marks_add("/A.OGG"); // 先成功持久化
    s_fail_save = true;
    CHECK(sync_marks_add("/A.OGG") == -2); // 已存在路径同样透出保存失败
    CHECK(sync_marks_has("/A.OGG"));
    CHECK(!sync_marks_last_save_ok());
}

static void test_capacity_boundary_exact(void)
{
    backend_reset();
    sync_marks_init(&s_store, NULL);
    char name[24];
    for (int i = 0; i < (int)SYNC_MARKS_MAX; i++) {
        make_name(name, sizeof(name), i);
        sync_marks_add(name);
    }
    CHECK(sync_marks_count() == sync_marks_capacity());
    make_name(name, sizeof(name), 0);
    CHECK(sync_marks_has(name)); // 恰好 256：无意外淘汰
    CHECK(sync_marks_add("REC_0000.OGG") == 1); // 已存在路径仍幂等
}

int main(void)
{
    RUN_TEST(test_add_has_count);
    RUN_TEST(test_duplicate_idempotent_and_refreshes_lru);
    RUN_TEST(test_lru_eviction_order);
    RUN_TEST(test_lru_eviction_respects_refresh);
    RUN_TEST(test_illegal_names_rejected);
    RUN_TEST(test_persist_and_reload_order);
    RUN_TEST(test_corrupt_blob_treated_empty);
    RUN_TEST(test_truncated_blob_treated_empty);
    RUN_TEST(test_save_failure_keeps_runtime);
    RUN_TEST(test_refresh_save_failure_returns_minus2);
    RUN_TEST(test_capacity_boundary_exact);
    return test_report();
}
