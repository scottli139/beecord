// Beecord 手表 UI Toast（ui_toast.h，M5.3）。
// 顶部居中胶囊（DESIGN_SYSTEM §3：12px、1.6s/错误 3s、z=toast90）。
// 实现挂 lv_layer_top（跨页面生效）；熄屏时调用被吞掉（黑层在下、layer_top 在上，
// 弹出会漏光——M5.3 注记）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// 显示一条 toast（拷贝文案；重复调用覆盖上一条；非 LVGL 任务可调用，内部自锁）。
// err 语义 = 重要级（ux-device §7 M5.3 定案）：
//   err=false 普通提示（"已保存"）：1.6s；熄屏时吞掉防漏光（此时屏幕天然已亮）。
//   err=true  错误/重要通知（无卡/存储满/录音失败/断电恢复）：3s；
//             熄屏时先唤醒屏幕再弹——这类反馈是按键操作的即时响应或必须告知的异常。
void ui_toast_show(const char *text, bool err);

// 初始化（建对象挂 layer_top；ui_init 内调用）
void ui_toast_init(void);

#ifdef __cplusplus
}
#endif
