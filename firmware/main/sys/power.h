// Beecord 电源状态逻辑（power.h，M5.4 二阶段）。硬件无关纯逻辑：电量平滑 +
// 低电阈值判定（连续样本确认 + 迟滞），宿主可测，禁 esp_* 头。
// 实机适配与轮询在 power_axp.c。依据：ux-device §7（<10% 录音中自动保存停止 +
// 三短音；<15% 主页橙色）、M5_PLAN §5.1（AXP2101 + 电量平滑宿主可测）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// 事件位（power_feed_sample 返回值按位或）
#define POWER_EVT_NONE 0x00u
#define POWER_EVT_LOW_ON 0x01u  // 低电确认沿（首次进入，未充电且连续样本过阈值）
#define POWER_EVT_LOW_OFF 0x02u // 低电解除沿（迟滞恢复）

#define POWER_PCT_UNKNOWN 0xFFu // 显示态未知（无有效样本），调用方据此画"未知"而非伪造值

typedef struct {
    float ema_alpha;        // 平滑系数（0<α≤1；越小越钝）。默认 0.2
    uint8_t low_pct;        // 低电触发阈值（%，严格小于才触发——ux-device <10%）。默认 10
    uint8_t low_release_pct;// 低电解除阈值（%>触发值形成迟滞）。默认 13
    uint8_t low_confirm_n;  // 触发需连续样本数（防抖/防单点抖动）。默认 3
    uint8_t low_release_n;  // 解除需连续样本数。默认 5
} power_config_t;

typedef struct {
    power_config_t cfg;
    bool has_sample;      // 已有 ≥1 有效样本（之前显示未知）
    float ema;            // 平滑百分比
    uint8_t low_count;    // 连续低样本计数（充电中清零）
    uint8_t ok_count;     // 连续恢复样本计数
    bool low_confirmed;   // 带迟滞的低电状态（事件沿由此产生）
} power_t;

// 初始化（cfg 传 NULL 用默认：α=0.2，触发 10%/3 连，解除 13%/5 连）
void power_init(power_t *p, const power_config_t *cfg);

// 喂一次原始样本。raw_pct：0~100；charging：充电态（充电中不计低电）。
// 低电判定走原始样本+连续计数（EMA 仅用于显示平滑，不拖慢关机判定）；
// 触发为【严格小于】阈值（恰 10% 不触发，<15% 橙显的展示语义另在 UI 层）；
// raw 非法（NaN/越界）返回 NONE 且不改状态（读取失败降级=跳过本次）。
// 返回事件位（LOW_ON 沿 / LOW_OFF 沿）。
uint8_t power_feed_sample(power_t *p, float raw_pct, bool charging);

// 显示百分比 0~100；无有效样本返回 POWER_PCT_UNKNOWN
uint8_t power_display_pct(const power_t *p);

bool power_is_low(const power_t *p); // 带迟滞的低电确认态
bool power_is_known(const power_t *p);

#ifdef __cplusplus
}
#endif
