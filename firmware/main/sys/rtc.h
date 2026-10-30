// Beecord RTC 时间逻辑（rtc.h，M5.4 三阶段）。硬件无关纯逻辑：BCD 编解码、
// 日历校验（闰年）、本地日历 ↔ epoch 换算（含 UTC 偏移），禁 esp_* 头，宿主可测。
// 实机适配在 rtc_pcf.c（PCF85063）。协议语义见 BUZZLINK.md §8.4（v0.2）。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// 日历时间（RTC 存"本地日历时间"——epochMs+utcOffsetMin 换算结果）
typedef struct {
    uint16_t year; // 2000..2099（PCF85063 能力窗口）
    uint8_t month; // 1..12
    uint8_t day;   // 1..31（按年月校验）
    uint8_t hour;  // 0..23
    uint8_t minute;// 0..59
    uint8_t second;// 0..59
} rtc_datetime_t;

// TIME_SET 有效范围（BUZZLINK §8.4）：2024-01-01 .. 2099-12-31（秒级）
#define RTC_EPOCH_MIN_S 1704067200LL          // 2024-01-01T00:00:00Z
#define RTC_EPOCH_MAX_S 4102444799LL          // 2099-12-31T23:59:59Z
#define RTC_OFFSET_MIN_M (-720)               // -12h
#define RTC_OFFSET_MAX_M 840                  // +14h

uint8_t rtc_bin2bcd(uint8_t bin);
uint8_t rtc_bcd2bin(uint8_t bcd);

// 日历合法性（年月日/时分秒范围 + 闰年 2 月）
bool rtc_datetime_valid(const rtc_datetime_t *dt);

// epoch（秒，UTC 语义）↔ 日历（days-from-civil 算法，整型秒级，支持 2000..2099）。
// 假定输入已含 UTC 偏移（调用方换算）；返回 false = 超出支持窗口。
bool rtc_epoch_to_datetime(int64_t epoch_s, rtc_datetime_t *out);
int64_t rtc_datetime_to_epoch(const rtc_datetime_t *dt); // 非法输入返回 -1

// TIME_SET 语义入口：epochMs(UTC) + utcOffsetMin → 本地日历。
// 校验：epoch 秒在 [MIN,MAX]、偏移在 [-720,840]、换算结果落在 2000..2099。
bool rtc_time_set_convert(int64_t epoch_ms, int utc_offset_min, rtc_datetime_t *out_local);

// 1970 以来星期的周日=0（写 RTC weekday 寄存器用；蔡勒公式变体）
uint8_t rtc_weekday(const rtc_datetime_t *dt); // 0=周日..6=周六

#ifdef __cplusplus
}
#endif
