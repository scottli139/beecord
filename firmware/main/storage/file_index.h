// 录音文件索引与元数据（M4.3）：枚举 REC_*.*（.WAV/.OGG）、新→旧排序（id 字典序
// 降序）、分页（§5.2：limit 缺省 16、上限 16）、元数据（size/流式全文件 crc16/
// durationMs/format/sampleRate/channels）。
//
// 所有文件访问经回调注入（buzz_file_io_t 五件套：open/read_at/size/close/listdir）：
// 设备侧由 buzz_cmds 用 FATFS/VFS 实现，宿主测用内存假文件系统（见
// test_host/test_file_index.c）。本模块硬件无关：不 include 任何 esp_* 头，
// 零 malloc（全部静态缓冲），单实例（buzz_file_index_init 绑定一次 io 表）。
//
// durationMs 解析规则：
//   WAV = 解析 RIFF fmt/data chunk：dataSize*1000/byteRate，sampleRate/channels
//         取自 fmt chunk；头损坏 → durationMs=0、sampleRate/channels=0（容错不报错）。
//   OGG = 读文件尾 64 KB（不足全读），从后向前找最后一个 OggS 页（version 字节
//         必须为 0，且 页头+段表+段数据恰好到 EOF 才认），granule（页头偏移 6，
//         8B LE）→ durationMs = granule*1000/48000（RFC 7845：Opus granule 为
//         48 kHz 域）；sampleRate/channels 固定 16000/1（M3 封装参数固定，
//         不解析 OpusHead）。找不到合法页 → durationMs=0。
//   0 字节文件：size=0、crc16=空输入 CRC（0xFFFF）、durationMs=0，不得崩溃。
//
// 索引上限 BUZZ_INDEX_MAX_FILES：匹配文件数超出时仅保留先枚举到的条目（FAT
// 枚举序），total 仍报真实计数；超出部分客户端按空页终止翻页。P0 容量取 128
// （约 64 小时录音），动态索引归 M6 再评估。
#ifndef BUZZ_FILE_INDEX_H
#define BUZZ_FILE_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buzz_proto.h" // BUZZ_FILE_ID_MAX_LEN / BUZZ_OK 及错误码约定

#ifdef __cplusplus
extern "C" {
#endif

// 列表页大小上限（BUZZLINK.md §5.2：每页最多 16 条）
#define BUZZ_INDEX_PAGE_MAX 16u

// 索引容量（静态 id 槽位数，零 malloc 的代价；溢出行为见文件头注释）
#define BUZZ_INDEX_MAX_FILES 128u

// 本模块自有错误码（其余复用 buzz_proto.h 的 BUZZ_OK/BUZZ_ERR_NULL/BUZZ_ERR_ARG/
// BUZZ_ERR_SIZE）
#define BUZZ_INDEX_ERR_NOT_FOUND (-6) // 文件不存在（open 失败一律归此，含路径非法）
#define BUZZ_INDEX_ERR_IO (-7)        // 已打开文件的读/取大小失败

// 目录枚举交付回调：name 为条目名（不含路径），仅在回调体内有效。
typedef void (*buzz_file_dirent_cb_t)(void *cb_ctx, const char *name);

// 文件 IO 注入表（五件套）。ctx 为各回调共享的上下文（由实现方解释，可为 NULL）。
// 所有回调均在调用方（buzz 服务任务）上下文同步执行，实现方须保证其可重入安全
// （单文件句柄内串行使用即可）。
typedef struct {
    void *ctx;
    // 按路径只读打开（id 即文件名，路径拼接归实现方）。返回句柄或 NULL（失败）。
    void *(*open)(void *ctx, const char *path);
    // 从文件绝对偏移 off 处读 len 字节到 buf（pread 语义，句柄无游标状态）。
    // 返回实际读到的字节数（短读允许，0 = EOF）；<0 = 出错。
    long (*read_at)(void *ctx, void *fh, uint32_t off, uint8_t *buf, uint32_t len);
    // 文件大小（字节）；<0 = 出错。
    long (*size)(void *ctx, void *fh);
    // 关闭并释放句柄。
    void (*close)(void *ctx, void *fh);
    // 枚举目录全部条目，对每条调用 cb(cb_ctx, name)。返回交付条数；<0 = 出错。
    long (*listdir)(void *ctx, buzz_file_dirent_cb_t cb, void *cb_ctx);
} buzz_file_io_t;

// 列表页条目（BUZZLINK.md §8.2 files[] 元素）
typedef struct {
    char id[BUZZ_FILE_ID_MAX_LEN + 1u]; // 文件名（含扩展名，D1），NUL 结尾
    uint32_t size;                      // 文件字节数
    uint32_t durationMs;                // 解析失败（含 0 字节文件）为 0
} buzz_file_entry_t;

// 文件元数据（BUZZLINK.md §8.3）
typedef struct {
    uint32_t size;      // 文件字节数
    uint16_t crc16;     // 整文件流式 CRC-16/CCITT-FALSE（0 字节文件 = 0xFFFF）
    uint32_t durationMs;
    char format[12];    // "ogg-opus" / "wav-pcm"（容器-编码，见 buzz_cmds 决策注记）
    uint32_t sampleRate; // WAV 取自 fmt chunk（损坏为 0）；OGG 固定 16000
    uint32_t channels;  // WAV 取自 fmt chunk（损坏为 0）；OGG 固定 1
} buzz_file_info_t;

// 绑定 io 表（结构体拷贝）并复位模块状态；单实例，重复调用重新绑定。
// io 或其任一回调为 NULL 返回 BUZZ_ERR_NULL。
int buzz_file_index_init(const buzz_file_io_t *io);

// 重建索引（重新枚举 + 过滤 + id 降序排序），取排序后 [offset, offset+limit)
// 切片并填齐每个条目的 size/durationMs。limit 规则：0 = 缺省 16，>16 截为 16
// （§5.2）。entries/entries_cap 由调用方提供，entries_cap 不得小于有效 limit。
// 输出：*out_total = 匹配文件真实总数（可大于索引容量），*out_count = 切片条数
// （offset ≥ 可用条数时为 0，返回仍 BUZZ_OK）。
// 返回 BUZZ_OK；参数非法返回 BUZZ_ERR_NULL/BUZZ_ERR_ARG，容量不足返回
// BUZZ_ERR_SIZE，枚举 IO 出错返回 BUZZ_INDEX_ERR_IO。
int buzz_file_index_list(uint32_t offset, uint32_t limit, buzz_file_entry_t *entries,
                         uint32_t entries_cap, uint32_t *out_total, uint32_t *out_count);

// 查询单个文件元数据：open → size → 流式全文件 crc16 → 按扩展名解析
// durationMs/format/sampleRate/channels → close。
// id 含 '/' 或 '\\' 返回 BUZZ_ERR_ARG（路径穿越防线，调用方亦应先过滤）；
// 文件不存在返回 BUZZ_INDEX_ERR_NOT_FOUND；读失败返回 BUZZ_INDEX_ERR_IO。
int buzz_file_index_info(const char *id, buzz_file_info_t *out);

// id 合法性校验（file_index 内部与调用方共用）：以 "REC_" 前缀 + .WAV/.OGG
// 扩展名（扩展名大小写不敏感）且长度 ≤ BUZZ_FILE_ID_MAX_LEN。
bool buzz_file_index_id_match(const char *name);

// 返回 init 时绑定的 io 表（指向模块静态存储）。buzz_dl 下载推流经此直接
// 读文件，保持"所有文件访问注入"的单一来源；未 init 返回 NULL。
const buzz_file_io_t *buzz_file_index_io(void);

// 整文件 CRC 计算/取缓存（FILE_INFO 与 DL_BEGIN 共享入口）：
// 先查 RAM 缓存（key = id + size，依据：录音文件写一次不改——recorder 只
// 新建递增名文件、从不改写已有文件；同名同 size 即同内容），命中零 IO；
// 未命中打开文件流式计算并入库。缓存容量 32 项（BUZZ_CRC_CACHE_CAP），
// 写满后顺序覆盖最旧槽。
// 返回 BUZZ_OK / BUZZ_ERR_NULL / BUZZ_INDEX_ERR_NOT_FOUND / BUZZ_INDEX_ERR_IO。
int buzz_file_index_crc16(const char *id, uint32_t size, uint16_t *out);

#ifdef __cplusplus
}
#endif

#endif // BUZZ_FILE_INDEX_H
