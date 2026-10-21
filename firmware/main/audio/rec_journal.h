// Beecord 录音 journal（rec_journal.h，M5.3，M5_PLAN D8 录音现场保护）。
// 断电恢复的数据契约：录音中每 5s 落盘一条 journal（当前文件/字节/时长/标记），
// 正常停止清除；开机检测到残留 journal → 按记录收尾（截断到已确认字节 +
// WAV 头长度回填）→ 文件可用并置"已恢复"标志供 UI toast。
//
// 本文件为纯逻辑（序列化/解析/恢复决策），禁 esp_* 头、禁文件 IO——
// 文件操作在 rec_journal_esp.c（目标侧），宿主单测注入内存缓冲验证。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define REC_JOURNAL_MAGIC 0x524A4E4Cu   // "RJNL"
#define REC_JOURNAL_VER 1u
#define REC_JOURNAL_FILE_MAX 64u        // 录音路径最大长度（含 NUL）
#define REC_JOURNAL_MARKS_MAX 64u       // 标记点上限（与 recorder 一致）
#define REC_JOURNAL_PATH_MAX 280u       // 挂载点 + 文件名拼接缓冲

// 一条 journal（磁盘与内存同构）
typedef struct {
    char file[REC_JOURNAL_FILE_MAX]; // 会话文件名（相对挂载点根，如 /REC_0021.OGG）
    uint8_t fmt;                     // 0=OGG 1=WAV（buzz_rec_fmt_t）
    uint8_t reserved[3];
    uint64_t bytes;                  // 已确认落盘字节（截断目标）
    uint32_t elapsed_ms;             // 会话时长（信息用途）
    uint32_t marks;                  // 标记数
    uint32_t mark_ms[REC_JOURNAL_MARKS_MAX]; // 标记时刻（会话内相对 ms）
} rec_journal_t;

// 序列化长度（定长，含头/校验）
// 8(头: magic4+ver2+len2) + 文件 64 + fmt/reserved 4 + bytes 8 + elapsed 4
// + marks 4 + mark_ms 256 + CRC 2 = 350
#define REC_JOURNAL_ENCODED_SIZE                                                 \
    (8u + REC_JOURNAL_FILE_MAX + 4u + 8u + 4u + 4u +                            \
     REC_JOURNAL_MARKS_MAX * 4u + 2u)

// 序列化：magic/ver/len + 字段 + CRC16（proto crc16 直连型）。返回写入长度；
// 缓冲不足返回 -1。编码后结构定长且字段对齐无依赖（全按字节拼装）。
int rec_journal_encode(const rec_journal_t *j, uint8_t *buf, uint32_t cap);

// 解析+校验（magic/ver/len/CRC）。返回 0；任何不一致返回 -1。
int rec_journal_decode(const uint8_t *buf, uint32_t len, rec_journal_t *out);

// 恢复决策（纯函数）：
// 输入 journal 与实际文件大小，输出该文件应截断到的字节数：
// - 文件比 journal 大 → 尾部为断电撕毁数据，截断到 j->bytes；
// - 文件大小恰好/更小 → 无法确认更多数据，保持原样（*out_truncate = 实际大小）。
// 返回 true = 文件可收尾（含 WAV 头回填由调用方按 fmt 处理）；
// false = journal 无效或文件缺失（调用方仅 toast 提示，不动文件）。
// actual_size 为 0 表示文件不存在。
bool rec_journal_plan_recovery(const rec_journal_t *j, uint64_t actual_size,
                               uint64_t *out_truncate_to);

// WAV 头长度回填（纯函数）：把 44 字节标准头中 data chunk 长度与 RIFF 总长
// 按数据字节数重算（仅对 fmt==WAV 的恢复有意义）。hdr 须为 buzz_wav_open 产物。
void rec_journal_wav_patch_header(uint8_t *hdr44, uint64_t data_bytes);

#ifdef __cplusplus
}
#endif
