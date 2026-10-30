// Beecord 已同步标记集合（sync_marks.h，M5.4，M5_PLAN D3）。
// 硬件无关纯逻辑：DL_END 推流完整结束才标记（buzz_dl 调用方保证语义），
// 文件名集合上限 256、LRU 淘汰（重下/重复标记刷新为最新），存储回调注入
// （目标侧 NVS blob 适配在 sync_marks_esp.c，宿主单测注入内存表）。禁 esp_* 头。
//
// 持久化格式（blob，定长头 + 变长串）：[u16 LE count][u8 len + len 字节]×count。
// 新增/刷新/淘汰后整表重写；blob 超过后端容量时 save 失败——运行态完整保留、
// 持久态为上次成功版本（sync_marks_last_save_ok 观测）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define SYNC_MARKS_MAX 256u                          // 集合上限（M5_PLAN D3）
#define SYNC_MARKS_NAME_MAX 32u                      // 与 BUZZ_FILE_ID_MAX_LEN 对齐

// 顺序语义：运行时 entries[0] 最久未用（淘汰候选），entries[count-1] 最新。
typedef struct {
    int (*load)(void *ctx, uint8_t *buf, uint32_t cap); // 返回 blob 长度；<0 = 无
    int (*save)(void *ctx, const uint8_t *buf, uint32_t len); // 0 成功 / <0 失败
} sync_marks_store_t;

// 初始化并装载持久态（坏 blob 视为空集合，不回写；后端 NULL = 纯运行态）
void sync_marks_init(const sync_marks_store_t *store, void *ctx);

// 打标记（DL_END 完整结束调用方保证）。返回：0=新增 1=已存在并刷新为最新
// -1=非法 name（NULL/空/超长） -2=持久化失败（运行态仍已更新）
int sync_marks_add(const char *name);

bool sync_marks_has(const char *name);
uint32_t sync_marks_count(void);
uint32_t sync_marks_capacity(void);

// 取集合中第 idx 个（0=最老；用于调试/导出）
const char *sync_marks_at(uint32_t idx);

bool sync_marks_last_save_ok(void);

#ifdef __cplusplus
}
#endif
