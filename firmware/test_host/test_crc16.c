// CRC-16/CCITT-FALSE 单测：BUZZLINK §3.2 自测向量 + 增量喂入一致性。
#include <stdint.h>

#include "crc16.h"
#include "test.h"

static void test_check_vectors(void)
{
    static const uint8_t s[] = "123456789";
    static const uint8_t b[] = "Beecord";
    CHECK_EQ_INT(buzz_crc16(s, 9), 0x29B1);
    CHECK_EQ_INT(buzz_crc16(b, 7), 0x5D1E);
}

static void test_empty_input(void)
{
    CHECK_EQ_INT(buzz_crc16(NULL, 0), BUZZ_CRC16_INIT); // 空输入 = 初值
    CHECK_EQ_INT(buzz_crc16((const uint8_t *)"", 0), 0xFFFF);
}

static void test_chunked_feed(void)
{
    static const uint8_t s[] = "123456789";
    uint16_t crc = BUZZ_CRC16_INIT;
    crc = buzz_crc16_update(crc, s, 1);      // "1"
    crc = buzz_crc16_update(crc, s + 1, 3);  // "234"
    crc = buzz_crc16_update(crc, s + 4, 0);  // 零长喂入不影响结果
    crc = buzz_crc16_update(crc, s + 4, 5);  // "56789"
    CHECK_EQ_INT(crc, 0x29B1);
}

int main(void)
{
    RUN_TEST(test_check_vectors);
    RUN_TEST(test_empty_input);
    RUN_TEST(test_chunked_feed);
    return test_report();
}
