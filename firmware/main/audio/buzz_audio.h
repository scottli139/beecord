// Beecord 音频采集封装（M2）：BSP 双麦（ES7210）初始化与 codec_dev 会话管理。
//
// 初始化序列（BSP 2.0.0 内部惰性完成 I2C/I2S，与官方 05_Spec_Analyzer 同路径）：
//   bsp_audio_codec_microphone_init() → esp_codec_dev_open(16kHz/16bit/双声道)
//   → esp_codec_dev_set_in_gain(33.0)。
// ES7210 为 I2S Slave，MCLK 由 BSP 供给；双麦默认开 MIC1|MIC2，无需改寄存器。
#pragma once

#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 采集格式（对齐 M3 Opus 终态）：立体声采集，混单声道在写入前由 buzz_recorder 完成
#define BUZZ_AUDIO_SAMPLE_RATE 16000u
#define BUZZ_AUDIO_BITS 16u
#define BUZZ_AUDIO_CHANNELS 2u
// 实测定案（M2.4 上板扫频）：24dB 偏低、36dB 近讲削顶，33dB 远近兼顾零削顶
// 开机默认麦克风增益（M3 实机标定：36 近距离爆音、33 正常；M5.3 电平标定复确认）。
// NVS 持久化归 M5.4（M5_PLAN §9 登记）；REPL `gain` 命令运行期可覆盖。
#define BUZA_MIC_GAIN_DEFAULT_DB 33.0f
#define BUZZ_AUDIO_IN_GAIN_DB BUZA_MIC_GAIN_DEFAULT_DB // 旧名别名，逐步收敛

// 打开麦克风采集：BSP 初始化（幂等）+ esp_codec_dev_open + 设置输入增益。
// 成功返回 ESP_OK 且 *out_handle 有效（应用生命周期内保持 open 直接读）；
// out_handle 为 NULL 返回 ESP_ERR_INVALID_ARG，BSP/codec 失败返回对应错误码。
esp_err_t buzz_audio_open_mic(esp_codec_dev_handle_t *out_handle);

// 关闭采集（codec close，实例保留，可再次 open 复用）。M2 录音循环不用，供后续里程碑回放冲突时使用。
esp_err_t buzz_audio_close_mic(esp_codec_dev_handle_t handle);

// 反初始化：close 并删除 codec 实例（esp_codec_dev_delete），句柄此后失效。
// 注意：BSP 未暴露 I2S/I2C 总线下电接口，总线本身保持初始化状态。
esp_err_t buzz_audio_deinit_mic(esp_codec_dev_handle_t handle);

// 运行期设置麦克风增益（ES7210 PGA，0~37.5dB，3dB 步进就近取档）。
// 供 REPL gain 命令调测；未 open 返回 ESP_ERR_INVALID_STATE，超范围返回 ESP_ERR_INVALID_ARG。
esp_err_t buzz_audio_set_in_gain(float db);

#ifdef __cplusplus
}
#endif
