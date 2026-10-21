// Beecord 录音页（scr_rec.h，M5.3）。
// 模态全屏页（ux-device §5.2 录音中查看）：大计时（timer 56）+ REC 脉冲 +
// 24 根电平条（RMS 历史带， honesty 说明见 .c）+ 暂停/继续 + 按住停止 800ms +
// 标记刻度条 + 无卡红条。数据轮询 recorder 实时统计（200ms）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "audio/buzz_recorder.h"

// 创建录音页 screen（ui_init 调用一次）
void scr_rec_create(void);

// 模态显隐（停止后回进入前页面）
void scr_rec_show(bool show);

// 录音事件（msg_pump 转发）：停止/自动停止时退出页面；暂停/继续即时反映
void scr_rec_on_rec_event(buzz_rec_evt_t evt);

// 无卡红条显隐（msg_pump 或轮询驱动；录音中拔卡靠写错误事件，此条覆盖"开始即无卡"）
void scr_rec_set_no_card(bool no_card);

#ifdef __cplusplus
}
#endif
