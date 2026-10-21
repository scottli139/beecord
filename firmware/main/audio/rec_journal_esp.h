// Beecord 录音 journal 目标侧文件胶（rec_journal_esp.h/.c，M5.3）。
// 录音中每 5s 落盘、正常停止清除、开机残留检测与文件收尾（截断 + WAV 头回填）。
// 文件布局：/sdcard/RECJRNL（定长 REC_JOURNAL_ENCODED_SIZE 字节）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "rec_journal.h"

// 录音会话期调用：journal 落盘（覆写）。recorder 每 5s 调一次。
esp_err_t rec_journal_save(const rec_journal_t *j);

// 正常停止清除 journal。文件不存在视为成功（幂等）。
esp_err_t rec_journal_clear(void);

// 开机检测（SD 挂载后调用一次）：
// 残留 journal → 按记录截断文件到已确认字节（WAV 另做头长度回填）→ 清除 journal。
// 有残留且文件可收尾时返回 ESP_OK 且 out_recovered_file 写入文件名（相对根）、
// recovered=true；无残留返回 ESP_OK 且 recovered=false；journal 损坏/文件缺失
// 返回错误（调用方决定 toast 文案，不清 journal 以便人工排查）。
esp_err_t rec_journal_check_recovery(char *out_recovered_file, size_t file_cap,
                                     bool *recovered);

#ifdef __cplusplus
}
#endif
