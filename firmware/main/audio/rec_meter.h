// Beecord 录音电平统计（rec_meter.h，M5.3）。
// 纯算法（禁 esp_* 头，宿主可测）：喂单声道 int16 PCM 帧 → 0..24 档平滑电平。
// 映射：RMS 归一化后开方 ×24（近似对数听感，计算廉价）；
// 平滑：上行快（逼近新值一半），下行慢（每帧至多退 1 档）——
// 视觉跟随 §3 电平条（24 根，高 6%~100%）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define REC_METER_LEVEL_MAX 24u

// dB 映射标定（M5.3 实机 meter-diag 实测定标）：
//   地板 -67dB（rms≈15，压掉 33dB 增益下的底噪）→ 0 档；
//   天花板 -23dB（rms≈2400，留削波余量）→ 24 档。
// 正常说话 rms 50~100（-56~-50dB）→ 6~10 档，大声 14+。
#define REC_METER_DB_FLOOR (-67.0)
#define REC_METER_DB_CEIL (-23.0)

typedef struct {
    uint8_t decay_per_feed; // 每帧下行最大档数（默认 1，视觉拖尾）
} rec_meter_config_t;

typedef struct {
    rec_meter_config_t cfg;
    uint8_t level;  // 平滑后档位 0..REC_METER_LEVEL_MAX
    uint8_t target; // 最近一帧未平滑目标档（窗口峰值采样用）
} rec_meter_t;

// 初始化（cfg 传 NULL 用默认）
void rec_meter_init(rec_meter_t *meter, const rec_meter_config_t *cfg);

// 复位电平到 0（会话切换用）
void rec_meter_reset(rec_meter_t *meter);

// 最近一帧的未平滑目标档（窗口峰值采样用；会话初/未喂过为 0）
uint8_t rec_meter_last_target(const rec_meter_t *meter);

// 喂入一帧 PCM，返回平滑后的当前档位（0..24）。
// 帧内 RMS 用 int64 累加防溢出；空帧（nsamples=0）只作用下行平滑。
uint8_t rec_meter_feed(rec_meter_t *meter, const int16_t *pcm, uint32_t nsamples);

#ifdef __cplusplus
}
#endif
