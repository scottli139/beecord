// rtc 时间逻辑宿主单测（M5.4 三阶段，BUZZLINK v0.2 §8.4）：BCD、日历校验、
// epoch↔本地日历换算（时区/闰年/边界）、TIME_SET 语义、json i64 解析。
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../main/proto/buzz_proto.h"
#include "../main/sys/rtc.h"
#include "buzz_server.h"
#include "test.h"

static void test_bcd_roundtrip(void)
{
    for (int v = 0; v < 100; v++) {
        CHECK(rtc_bcd2bin(rtc_bin2bcd((uint8_t)v)) == v);
    }
    CHECK(rtc_bin2bcd(59u) == 0x59u);
    CHECK(rtc_bcd2bin(0x23u) == 23u);
}

static void test_datetime_validation(void)
{
    rtc_datetime_t dt = { .year = 2026, .month = 10, .day = 30, .hour = 12 };
    CHECK(rtc_datetime_valid(&dt));
    dt.month = 13;
    CHECK(!rtc_datetime_valid(&dt));
    dt.month = 2; dt.day = 29; dt.year = 2023; // 非闰年
    CHECK(!rtc_datetime_valid(&dt));
    dt.year = 2024; // 闰年
    CHECK(rtc_datetime_valid(&dt));
    dt.year = 2000; // 世纪闰年
    CHECK(rtc_datetime_valid(&dt)); // 2000 为世纪闰年（%400==0）
    dt.year = 2026; dt.day = 31; dt.month = 4; // 4 月无 31
    CHECK(!rtc_datetime_valid(&dt));
    dt.month = 1; dt.hour = 24;
    CHECK(!rtc_datetime_valid(&dt));
    dt.hour = 0; dt.minute = 60;
    CHECK(!rtc_datetime_valid(&dt));
    dt.minute = 0; dt.year = 1999; // 窗口外
    CHECK(!rtc_datetime_valid(&dt));
    dt.year = 2100;
    CHECK(!rtc_datetime_valid(&dt));
}

static void test_epoch_anchor(void)
{
    // 2024-01-01T00:00:00Z = 1704067200（UTC 锚点）
    rtc_datetime_t dt = { .year = 2024, .month = 1, .day = 1 };
    CHECK(rtc_datetime_to_epoch(&dt) == 1704067200LL);
    rtc_datetime_t back;
    CHECK(rtc_epoch_to_datetime(1704067200LL, &back));
    CHECK(back.year == 2024 && back.month == 1 && back.day == 1 && back.hour == 0);
    // 2099-12-31T23:59:59Z 窗口上界
    rtc_datetime_t top = { .year = 2099, .month = 12, .day = 31, .hour = 23,
                           .minute = 59, .second = 59 };
    CHECK(rtc_datetime_to_epoch(&top) == 4102444799LL);
    CHECK(rtc_epoch_to_datetime(4102444799LL, &back));
    CHECK(back.year == 2099 && back.month == 12 && back.day == 31);
    // 窗口外失败
    CHECK(!rtc_epoch_to_datetime(4102444800LL, &back)); // 2100 超出窗口
    // 注：2023 末在 rtc_epoch_to_datetime 的 2000..2099 契约内（合法）；
    // TIME_SET 侧窗口由 rtc_time_set_convert 的 RTC_EPOCH_MIN_S 拦截（见下）
}

static void test_epoch_roundtrip_samples(void)
{
    const int64_t samples[] = { 1704067200LL, 1735689600LL, 1767225600LL,
                                1893456000LL, 4070908800LL };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        rtc_datetime_t dt;
        CHECK(rtc_epoch_to_datetime(samples[i], &dt));
        CHECK(rtc_datetime_valid(&dt));
        CHECK(rtc_datetime_to_epoch(&dt) == samples[i]);
    }
}

static void test_time_set_timezone(void)
{
    // UTC 2024-01-01T00:00:00Z + 480min(UTC+8) = 本地 2024-01-01 08:00
    rtc_datetime_t local;
    CHECK(rtc_time_set_convert(1704067200000LL, 480, &local));
    CHECK(local.year == 2024 && local.month == 1 && local.day == 1);
    CHECK(local.hour == 8 && local.minute == 0 && local.second == 0);
    // 负偏移（美东 -300min）：UTC 00:30 → 本地前一日 19:30
    CHECK(rtc_time_set_convert(1704067200000LL + 30u * 60000u, -300, &local));
    CHECK(local.month == 12 && local.day == 31 && local.hour == 19 && local.minute == 30);
    // +14h 边界（莱恩线）
    CHECK(rtc_time_set_convert(1704067200000LL, 840, &local));
    CHECK(local.hour == 14);
    // -12h 边界：UTC 00:00 - 12h = 前一日 12:00（正午，非零点）
    CHECK(rtc_time_set_convert(1704067200000LL, -720, &local));
    CHECK(local.hour == 12 && local.day == 31 && local.month == 12);
}

static void test_time_set_range_rejected(void)
{
    rtc_datetime_t local;
    // epoch 越界（v0.2 §8.4：2024-01-01..2099-12-31）
    CHECK(!rtc_time_set_convert(1704067199999LL, 0, &local)); // 2023 末
    CHECK(!rtc_time_set_convert(4102444800000LL, 0, &local)); // 2100 初
    // 偏移越界（-720..+840）
    CHECK(!rtc_time_set_convert(1704067200000LL, 841, &local));
    CHECK(!rtc_time_set_convert(1704067200000LL, -721, &local));
    // 2099 末 +14h 本地越界（2099-12-31 23:59 +14h → 2100）
    CHECK(!rtc_time_set_convert(4102444799000LL, 840, &local));
}

static void test_weekday(void)
{
    // 1970-01-01 周四为基准外推：2024-01-01 = 周一
    rtc_datetime_t dt = { .year = 2024, .month = 1, .day = 1 };
    CHECK(rtc_weekday(&dt) == 1u); // 周一
    rtc_datetime_t sun = { .year = 2024, .month = 1, .day = 7 };
    CHECK(rtc_weekday(&sun) == 0u); // 周日
}

// ---- buzz_json_get_i64（TIME_SET epochMs 解析，固件端与 Sim 端共用规则） ----
static void test_json_i64(void)
{
    int64_t v = 0;
    CHECK(buzz_json_get_i64("{\"epochMs\":1704067200000}", "epochMs", &v) == BUZZ_OK);
    CHECK(v == 1704067200000LL);
    CHECK(buzz_json_get_i64("{\"epochMs\":-1234567890123}", "epochMs", &v) == BUZZ_OK);
    CHECK(v == -1234567890123LL);
    CHECK(buzz_json_get_i64("{\"a\":1}", "epochMs", &v) == BUZZ_JSON_ERR_NOT_FOUND);
    CHECK(buzz_json_get_i64("{\"epochMs\":\"x\"}", "epochMs", &v) == BUZZ_JSON_ERR_MALFORMED);
    CHECK(buzz_json_get_i64("{\"epochMs\":17x}", "epochMs", &v) == BUZZ_JSON_ERR_MALFORMED);
    // 超 int64：ERANGE → malformed
    CHECK(buzz_json_get_i64("{\"epochMs\":99999999999999999999}", "epochMs", &v) ==
          BUZZ_JSON_ERR_MALFORMED);
}

int main(void)
{
    RUN_TEST(test_bcd_roundtrip);
    RUN_TEST(test_datetime_validation);
    RUN_TEST(test_epoch_anchor);
    RUN_TEST(test_epoch_roundtrip_samples);
    RUN_TEST(test_time_set_timezone);
    RUN_TEST(test_time_set_range_rejected);
    RUN_TEST(test_weekday);
    RUN_TEST(test_json_i64);
    return test_report();
}
