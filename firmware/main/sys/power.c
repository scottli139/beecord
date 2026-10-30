// Beecord 电源状态逻辑实现（power.c，M5.4 二阶段）。
#include "power.h"

#include <math.h>
#include <stddef.h>

static const power_config_t s_default_cfg = {
    .ema_alpha = 0.2f,
    .low_pct = 10u,
    .low_release_pct = 13u,
    .low_confirm_n = 3u,
    .low_release_n = 5u,
};

void power_init(power_t *p, const power_config_t *cfg)
{
    if (p == NULL) {
        return;
    }
    p->cfg = (cfg != NULL) ? *cfg : s_default_cfg;
    p->has_sample = false;
    p->ema = 0.0f;
    p->low_count = 0;
    p->ok_count = 0;
    p->low_confirmed = false;
}

uint8_t power_feed_sample(power_t *p, float raw_pct, bool charging)
{
    if (p == NULL || !isfinite(raw_pct) || raw_pct < 0.0f || raw_pct > 100.0f) {
        return POWER_EVT_NONE; // 读取失败/非法样本：跳过（降级，不 abort）
    }
    if (!p->has_sample) {
        p->ema = raw_pct; // 首个样本直接取（避免从 0 爬坡）
        p->has_sample = true;
    } else {
        p->ema += (raw_pct - p->ema) * p->cfg.ema_alpha;
    }

    uint8_t events = POWER_EVT_NONE;
    // 低电判定用【原始样本】+连续计数防抖（EMA 只服务显示——EMA 级联会把
    // 真实下跌钝化成 ~1/α 拍后才过阈值，低电关机被拖慢，M5.4 单测实锤）。
    const uint8_t raw = (uint8_t)(raw_pct + 0.5f);
    if (charging) {
        p->low_count = 0; // 充电中不判低电；恢复计数照常走
        p->ok_count++;
    } else if (!p->low_confirmed && raw < p->cfg.low_pct) { // 严格小于（ux-device <10%）
        p->low_count++;
        p->ok_count = 0;
    } else if (p->low_confirmed && raw >= p->cfg.low_release_pct) {
        p->ok_count++;
        p->low_count = 0;
    } else {
        // 迟滞带内（未确认时 >阈值 / 已确认时 <解除阈值）：两侧计数清零，
        // 避免边界抖动时两计数同时累积
        p->low_count = 0;
        p->ok_count = 0;
    }

    if (!p->low_confirmed && !charging && p->low_count >= p->cfg.low_confirm_n) {
        p->low_confirmed = true;
        events |= POWER_EVT_LOW_ON;
    } else if (p->low_confirmed && p->ok_count >= p->cfg.low_release_n) {
        p->low_confirmed = false;
        events |= POWER_EVT_LOW_OFF;
    }
    return events;
}

uint8_t power_display_pct(const power_t *p)
{
    if (p == NULL || !p->has_sample) {
        return POWER_PCT_UNKNOWN;
    }
    uint8_t pct = (uint8_t)(p->ema + 0.5f);
    return pct > 100u ? 100u : pct;
}

bool power_is_low(const power_t *p)
{
    return p != NULL && p->low_confirmed;
}

bool power_is_known(const power_t *p)
{
    return p != NULL && p->has_sample;
}
