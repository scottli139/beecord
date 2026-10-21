// Beecord 录音电平统计实现（rec_meter.c，M5.3）。
#include "rec_meter.h"

#include <math.h>
#include <stddef.h>

static const rec_meter_config_t s_default_cfg = {
    .decay_per_feed = 1,
};

void rec_meter_init(rec_meter_t *meter, const rec_meter_config_t *cfg)
{
    if (meter == NULL) {
        return;
    }
    meter->cfg = (cfg != NULL) ? *cfg : s_default_cfg;
    if (meter->cfg.decay_per_feed == 0) {
        meter->cfg.decay_per_feed = 1;
    }
    meter->level = 0;
    meter->target = 0;
}

void rec_meter_reset(rec_meter_t *meter)
{
    if (meter != NULL) {
        meter->level = 0;
        meter->target = 0;
    }
}

// 帧 RMS → 目标档：dB 映射（地板/天花板由 REC_METER_DB_FLOOR/CEIL 配置）。
// sqrt 映射会把语音常用动态段（-30~-10dB）压在中高位、视觉上"等高无变化"
//（M5.3 实机观测）——dB 线性摊开低段，说话起伏可见。int64 累加防溢出。
static uint8_t frame_level(const int16_t *pcm, uint32_t nsamples)
{
    if (pcm == NULL || nsamples == 0) {
        return 0;
    }
    int64_t acc = 0;
    for (uint32_t i = 0; i < nsamples; i++) {
        acc += (int32_t)pcm[i] * (int32_t)pcm[i];
    }
    const double rms = sqrt((double)acc / (double)nsamples);
    if (rms < 1.0) {
        return 0;
    }
    const double norm = rms / 32767.0;
    const double db = 20.0 * log10(norm);
    // 地板/天花板可配（M5.3 实机定标：-67dB 地板压底噪、-23dB 天花留余量）
    const double span = REC_METER_DB_CEIL - REC_METER_DB_FLOOR;
    const double scaled = (db - REC_METER_DB_FLOOR) / span * (double)REC_METER_LEVEL_MAX;
    if (scaled <= 0.0) {
        return 0;
    }
    uint32_t lvl = (uint32_t)(scaled + 0.5);
    return (uint8_t)(lvl > REC_METER_LEVEL_MAX ? REC_METER_LEVEL_MAX : lvl);
}

uint8_t rec_meter_feed(rec_meter_t *meter, const int16_t *pcm, uint32_t nsamples)
{
    if (meter == NULL) {
        return 0;
    }
    const uint8_t target = frame_level(pcm, nsamples);
    meter->target = target;
    uint8_t cur = meter->level;
    if (target > cur) {
        // 上行快：每帧逼近差值一半（视觉立即响应）
        cur = (uint8_t)(cur + (target - cur + 1) / 2);
    } else if (target < cur) {
        // 下行慢：拖尾退档（decay_per_feed 未初始化兜底为 1，防电平冻结）
        uint8_t decay = meter->cfg.decay_per_feed;
        if (decay == 0) {
            decay = 1;
        }
        uint8_t drop = (uint8_t)(cur - target);
        if (drop > decay) {
            drop = decay;
        }
        cur = (uint8_t)(cur - drop);
    }
    meter->level = cur;
    return cur;
}

uint8_t rec_meter_last_target(const rec_meter_t *meter)
{
    return meter != NULL ? meter->target : 0;
}
