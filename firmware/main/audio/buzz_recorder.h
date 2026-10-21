// Beecord 录音采集任务（M2 建立，M3 双格式，M5.3 录音可靠性 + 热键支撑）：
// codec 读取 → L/R 混单声道 → Opus+Ogg（.OGG）或 WAV（.WAV）写 TF 卡。
//
// 线程模型：本模块创建单个采集任务（prio 5 / 栈 8KB）长期运行；
// 控制方经命令队列下发 start/stop/pause/resume/mark，阻塞握手（带超时）；
// 会话期间经事件回调（buzz_recorder_set_event_cb，采集任务上下文触发，须非阻塞）
// 向 UI 上报 started/stopped/paused/resumed/marked/error；UI 侧用实时统计 getter
// 轮询（原子字宽，任意任务可读）。静态缓冲、启动期建队列与任务，采集循环内零 malloc。
//
// M5.3 新增：暂停/继续（暂停期读块丢弃、计时冻结）、标记点（会话内相对时刻，
// 最多 64 个）、电平统计（rec_meter，0..24 档）、每 5s journal 落盘钩子
//（rec_journal_esp）、码率参数（设置页两档的前置）、写满/写错误事件细分。
//
// 文件名 /sdcard/REC_%04d.OGG 或 .WAV 递增：开录前扫目录同时认两种后缀
// 取最大序号 +1（卡在他机写过不冲突）。
#pragma once

#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 录音封装格式：OGG 为 M3 起缺省（Opus VBR），WAV 为 M2 原始 PCM 格式
typedef enum {
    BUZZ_REC_FMT_OGG = 0,
    BUZZ_REC_FMT_WAV,
} buzz_rec_fmt_t;

typedef enum {
    BUZZ_REC_STATE_IDLE = 0,
    BUZZ_REC_STATE_RECORDING,
    BUZZ_REC_STATE_PAUSED, // M5.3：暂停中（mic 仍读但丢弃，计时冻结）
} buzz_rec_state_t;

// 会话事件（采集任务上下文回调；实现必须非阻塞——UI 侧转 ui_msg_post）
typedef enum {
    BUZZ_REC_EVT_STARTED = 0,   // 开录成功（message=路径）
    BUZZ_REC_EVT_STOPPED,       // 正常停止（message=路径）
    BUZZ_REC_EVT_PAUSED,
    BUZZ_REC_EVT_RESUMED,
    BUZZ_REC_EVT_MARKED,        // value=标记序号（1 起）
    BUZZ_REC_EVT_ERROR,         // 异常中止：读失败/写错误（message=原因）
    BUZZ_REC_EVT_AUTO_STOPPED,  // 写满自动停止（message=路径；存储异常行为 §7）
} buzz_rec_evt_t;

typedef void (*buzz_rec_event_cb_t)(buzz_rec_evt_t evt, const char *message,
                                    int32_t value, void *ctx);

// 初始化：记录 mic 句柄、创建命令/应答队列与采集任务，并预扫一次目录取下一序号。
// 需在 TF 卡挂载（buzz_sdcard_probe）与 buzz_audio_open_mic 成功后调用；
// mic 为 NULL 返回 ESP_ERR_INVALID_ARG。
esp_err_t buzz_recorder_init(esp_codec_dev_handle_t mic);

// 开始录音（fmt 指定封装格式）：阻塞至采集任务完成开录握手（超时 10s）。
// 已在录音中（含暂停）/ 任务未创建 / 文件打开失败返回相应错误码。
esp_err_t buzz_recorder_start(buzz_rec_fmt_t fmt);

// 停止录音：阻塞至采集任务完成停录握手（WAV 长度回填 + fclose + 统计回报）。
// 空闲态调用返回 ESP_ERR_INVALID_STATE（调用方应先行查状态给出友好提示）。
esp_err_t buzz_recorder_stop(void);

// M5.3：暂停 / 继续 / 打标记（阻塞握手，仅在录音态有效；暂停态 mark 拒绝）
esp_err_t buzz_recorder_pause(void);
esp_err_t buzz_recorder_resume(void);
esp_err_t buzz_recorder_mark(void);

// M5.3：会话事件回调（采集任务上下文触发；NULL 清除）
void buzz_recorder_set_event_cb(buzz_rec_event_cb_t cb, void *ctx);

// 当前录音状态（任意任务调用，枚举读写天然原子）
buzz_rec_state_t buzz_recorder_state(void);

// ---------- 实时统计（LVGL 轮询读，原子字宽；仅在会话期间有意义） ----------
uint32_t buzz_recorder_elapsed_ms(void);      // 会话累计时长（不含暂停）
uint8_t buzz_recorder_level(void);            // 电平 0..24（rec_meter 平滑值）
// M5.3 窗口峰值：自上次读取以来的块目标峰值（读并复位到当前瞬时档）——
// UI 轮询取峰保留语音包络动态，比末值采样生动
uint8_t buzz_recorder_level_peak(void);
uint64_t buzz_recorder_bytes_written(void);   // 已写字节（落盘侧计数）
uint32_t buzz_recorder_mark_count(void);      // 已打标记数
uint32_t buzz_recorder_mark_ms(uint32_t idx); // 第 idx 个标记的会话内时刻（ms）
const char *buzz_recorder_current_path(void); // 当前会话路径（无会话返回 ""）

// M5.3 电平排查：原始块 RMS 诊断（最近/会话最小/最大；min 无数据时为 0）
void buzz_recorder_meter_diag(uint32_t *last_rms, uint32_t *min_rms, uint32_t *max_rms);

// M5.3：码率设置（bps，如 44000/24000；0 = 默认 BUZZ_REC_PIPE_BITRATE）。
// 作用于下一次 start 创建的 Opus 编码器；get 返回生效值（默认档返回 0）。
void buzz_recorder_set_bitrate(int bitrate_bps);
int buzz_recorder_get_bitrate(void);

#ifdef __cplusplus
}
#endif
