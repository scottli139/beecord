// 录音控制层（ui_rec.h，M5.3）。
// 全局录音热键的执行体：独立控制任务（prio 2）串行执行阻塞式 recorder API
// （start/stop 握手最长 10s，绝不能跑在 LVGL 任务上）。提示音在控制任务播放——
// 绝不能在采集任务播（阻塞 1s 会冲爆 mic DMA），也不占 LVGL 任务。
//
// 事件流：recorder 回调（采集任务上下文，非阻塞）→ 控制任务队列 →
//         ① 提示音（本任务） ② 转 ui_msg(REC)（LVGL 任务做页面/横幅/toast）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// 初始化：创建控制任务 + 注册 recorder 事件回调。须在 ui_init 之后调用
// （依赖 UI 消息队列；事件转发在 UI 就绪前安全丢弃）。
esp_err_t ui_rec_init(void);

// 热键/触控请求（非阻塞；队列满返回 ESP_ERR_TIMEOUT 由调用方忽略）。
// 前置检查（无卡/低容量/状态）在控制任务执行时做，结果经事件/toast 反馈。
esp_err_t ui_rec_request_toggle(void);      // BOOT 单击：开始 ⇄ 停止
esp_err_t ui_rec_request_pause_toggle(void); // BOOT 双击
esp_err_t ui_rec_request_mark(void);         // BOOT 长按 1.5s

// M5.4：显式停止（低电自动停止专用）。空闲态调用为空操作（绝不开始录音，
// 因此不能复用 toggle）；录音中（含暂停中）请求停止，三短音 + "已保存"反馈。
esp_err_t ui_rec_request_stop_low_battery(void);

// 开机恢复（main.c 检测置标志，UI 起来后消费一次弹 toast）
void ui_rec_set_recovered(const char *file);
bool ui_rec_take_recovered(char *out_file, size_t cap);

#ifdef __cplusplus
}
#endif
