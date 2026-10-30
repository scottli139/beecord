// Beecord 关机警告层（ui_shutdown.h，M5.4 二阶段）。
// PWR 长按 4.5s 触发（AXP2101 硬件 ~6s 断电前的软件告警窗口，ux-device §2）：
// 全屏纯黑 + 居中"即将关机…" + 剩余秒数（DESIGN_SYSTEM v1.6 §3 关机警告层 z80，
// 不可交互）。熄屏时也能显示（先点亮）。BOOT 的 4.5s 事件不得触发（调用方保证）。
//
// 消退语义（keyd 无释放通知，M5.4 定案）：PWR 4.5s 后若 6s 内未硬件断电
//（用户松手），层在 ~2.5s 后自动消退；期间再次 PWR 4.5s 重置计时。
// 不声称可取消硬件关机。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 显示/重置关机警告层（幂等；任何任务可调用，内部自锁并点亮屏幕）
void ui_shutdown_warn_show(void);

#ifdef __cplusplus
}
#endif
