// Beecord 提示音 ESP 侧播放（beep_play.h，M5.2）。
// 序列逻辑在 beep.c（宿主可测）；本模块只做正弦 PCM 合成 + ES8311 播放。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

#include "beep.h"

// 播放事件提示音（同步阻塞；尊重 beep_is_enabled 开关）。
// 扬声器会话按需 open/write/close，与 ES7210 采集链路独立（codec 双设备）。
esp_err_t beep_play(beep_event_t evt);

// 自定义单音（REPL `beep t`）：声学特性刻画/音量听辨用，长期保留为调试手段。
// freq_hz 正弦频率，dur_ms 时长，vol 0~100 覆盖默认音量。
esp_err_t beep_play_tone(uint16_t freq_hz, uint16_t dur_ms, int vol);

#ifdef __cplusplus
}
#endif
