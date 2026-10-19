// Beecord 录音采集任务（M2）：codec 读取 → L/R 混单声道 → WAV 写 TF 卡。
//
// 线程模型：本模块创建单个采集任务（prio 5 / 栈 6KB）长期运行；
// REPL 任务经命令队列下发 start/stop，采集任务完成后经应答队列回报结果，
// start/stop 为阻塞握手（带超时），控制台可同步打印录音文件路径与统计。
// 静态缓冲、启动期建队列与任务，采集循环内零 malloc。
//
// 文件名 /sdcard/REC_%04d.WAV 递增：开录前扫目录取最大序号 +1（卡在他机写过不冲突）。
#pragma once

#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUZZ_REC_STATE_IDLE = 0,
    BUZZ_REC_STATE_RECORDING,
} buzz_rec_state_t;

// 初始化：记录 mic 句柄、创建命令/应答队列与采集任务，并预扫一次目录取下一序号。
// 需在 TF 卡挂载（buzz_sdcard_probe）与 buzz_audio_open_mic 成功后调用；
// mic 为 NULL 返回 ESP_ERR_INVALID_ARG。
esp_err_t buzz_recorder_init(esp_codec_dev_handle_t mic);

// 开始录音：阻塞至采集任务完成开录握手（超时 10s，覆盖目录扫描与卡抖动）。
// 成功返回 ESP_OK；已在录音中 / 任务未创建 / 文件打开失败返回相应错误码。
esp_err_t buzz_recorder_start(void);

// 停止录音：阻塞至采集任务完成停录握手（WAV 长度回填 + fclose + 统计回报）。
// 空闲态调用返回 ESP_ERR_INVALID_STATE（调用方应先行查状态给出友好提示）。
esp_err_t buzz_recorder_stop(void);

// 当前录音状态（供控制台展示；仅 REPL 任务调用，枚举读写天然原子）。
buzz_rec_state_t buzz_recorder_state(void);

#ifdef __cplusplus
}
#endif
