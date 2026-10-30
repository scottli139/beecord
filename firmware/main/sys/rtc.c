// Beecord RTC 时间逻辑实现（rtc.c，M5.4 三阶段）。
#include "rtc.h"

#include <stddef.h>

uint8_t rtc_bin2bcd(uint8_t bin)
{
    return (uint8_t)(((bin / 10u) << 4) | (bin % 10u));
}

uint8_t rtc_bcd2bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) & 0x0Fu) * 10u + (bcd & 0x0Fu));
}

// 格里高利闰年（2000..2099 窗口内规则一致）
static bool is_leap(uint16_t y)
{
    return (y % 4u == 0u && y % 100u != 0u) || (y % 400u == 0u);
}

static uint8_t days_in_month(uint16_t y, uint8_t m)
{
    static const uint8_t t[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2u && is_leap(y)) {
        return 29u;
    }
    return t[m - 1u];
}

bool rtc_datetime_valid(const rtc_datetime_t *dt)
{
    if (dt == NULL || dt->year < 2000u || dt->year > 2099u || dt->month < 1u ||
        dt->month > 12u || dt->day < 1u || dt->day > days_in_month(dt->year, dt->month) ||
        dt->hour > 23u || dt->minute > 59u || dt->second > 59u) {
        return false;
    }
    return true;
}

// days from civil（Howard Hinnant 算法）：1970-01-01 起的天数，纯整数。
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2u ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned mp = m > 2u ? m - 3u : m + 9u; // Hinnant 月映射：3 月起年为 0..11
    const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097LL + (int64_t)doe - 719468LL;
}

static bool civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    z += 719468LL;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned dd = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned mm = mp < 10u ? mp + 3u : mp - 9u; // 逆映射回 1..12
    *y = yy + (mm <= 2u ? 1 : 0);
    *m = mm;
    *d = dd;
    return *y >= 2000 && *y <= 2099; // 窗口守卫：换算出窗口外即失败
}

int64_t rtc_datetime_to_epoch(const rtc_datetime_t *dt)
{
    if (!rtc_datetime_valid(dt)) {
        return -1;
    }
    const int64_t days = days_from_civil(dt->year, dt->month, dt->day);
    return days * 86400LL + (int64_t)dt->hour * 3600LL + (int64_t)dt->minute * 60LL +
           dt->second;
}

bool rtc_epoch_to_datetime(int64_t epoch_s, rtc_datetime_t *out)
{
    if (out == NULL || epoch_s < 0) {
        return false;
    }
    const int64_t days = epoch_s / 86400LL;
    int64_t rem = epoch_s % 86400LL;
    int64_t y = 0;
    unsigned m = 0, d = 0;
    if (!civil_from_days(days, &y, &m, &d)) {
        return false;
    }
    out->year = (uint16_t)y;
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
    out->hour = (uint8_t)(rem / 3600LL);
    rem %= 3600LL;
    out->minute = (uint8_t)(rem / 60LL);
    out->second = (uint8_t)(rem % 60LL);
    return true;
}

bool rtc_time_set_convert(int64_t epoch_ms, int utc_offset_min, rtc_datetime_t *out_local)
{
    if (out_local == NULL || utc_offset_min < RTC_OFFSET_MIN_M ||
        utc_offset_min > RTC_OFFSET_MAX_M) {
        return false;
    }
    const int64_t epoch_s = epoch_ms / 1000LL; // 毫秒丢弃（RTC 秒级）
    if (epoch_s < RTC_EPOCH_MIN_S || epoch_s > RTC_EPOCH_MAX_S) {
        return false;
    }
    const int64_t local_s = epoch_s + (int64_t)utc_offset_min * 60LL;
    if (!rtc_epoch_to_datetime(local_s, out_local)) {
        return false;
    }
    // 本地落点也须在日历窗口内（如 2099 末 +14h 会越界）
    return rtc_datetime_valid(out_local);
}

uint8_t rtc_weekday(const rtc_datetime_t *dt)
{
    const int64_t epoch = rtc_datetime_to_epoch(dt);
    if (epoch < 0) {
        return 0;
    }
    return (uint8_t)((epoch / 86400LL + 4) % 7); // 1970-01-01 是周四
}
