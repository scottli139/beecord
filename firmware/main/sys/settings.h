// Beecord 设置框架（settings.h，M5.4）。
// 硬件无关纯逻辑：默认值表 + 运行值 + 存储回调注入（持久化后端可替换——
// 目标侧 ESP NVS 适配在 settings_esp.c，宿主单测注入内存表）。逻辑层禁 esp_* 头。
//
// 语义约定（M5_PLAN §5.1）：
// - 每个设置项有类型与默认值；启动 load_all 时无存储值的键保持默认（默认行为
//   与现状一致：质量 44kbps / 增益 33dB / 提示音开 / 亮度 100 / 熄屏 10s）。
// - set 立即写存储；写失败不丢运行值（运行态优先），last_save_ok 供上层观测告警。
// - 值域校验（集中在本模块，load 与 set 同规则，非法值拒绝并保留原值/默认）：
//   质量/提示音仅 0/1；亮度 0~100；熄屏时间离散档 {0,10,30,60}（0=永不，ux-device
//   设置项）；增益必须有限数且 0~37.5 dB（ES7210 PGA 量程）。
// - "持久化值"与"应用"分离：本模块只管存取；生效动作（调 recorder/gain/beep/
//   screen_pwr）由调用方在加载/修改后显式执行（亮度当前无应用路径，仅存值）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SETTINGS_REC_QUALITY = 0, // u8：0=标准 ~44kbps（默认） 1=省空间 ~24kbps
    SETTINGS_MIC_GAIN_DB,     // float：麦克风增益 dB（默认 33.0，ES7210 0~37.5）
    SETTINGS_BEEP_ENABLED,    // u8 bool：提示音开关（默认 1=开）
    SETTINGS_BRIGHTNESS,      // u8 0~100：亮度持久化值（默认 100；应用路径未接，见头注）
    SETTINGS_SCREEN_OFF_S,    // u8 秒：无操作自动熄屏（默认 10；0=永不）
    SETTINGS_ID_COUNT
} settings_id_t;

// 存储后端注入：load 返回实际字节数（<0 = 无此键/读错误，保持默认）；
// save 返回 0 成功 / <0 失败。key = settings_id_t。
typedef struct {
    int (*load)(void *ctx, uint8_t key, void *buf, uint32_t cap);
    int (*save)(void *ctx, uint8_t key, const void *buf, uint32_t len);
} settings_store_t;

// 初始化（绑定后端；store 为 NULL 则纯运行态、不持久化）
void settings_init(const settings_store_t *store, void *ctx);

// 启动装载：逐键 load，无键保持默认。后端 NULL 时为空操作。
void settings_load_all(void);

uint8_t settings_get_u8(settings_id_t id);
float settings_get_float(settings_id_t id);
void settings_set_u8(settings_id_t id, uint8_t value);
void settings_set_float(settings_id_t id, float value);

// 最近一次 set 的持久化是否成功（false 时运行值仍已生效；上层可告警/重试）
bool settings_last_save_ok(void);

// 测试辅助：复位全部运行值到默认（不动存储）
void settings_reset_to_defaults(void);

#ifdef __cplusplus
}
#endif
