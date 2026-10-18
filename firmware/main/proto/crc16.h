// BuzzLink 协议层 —— CRC-16/CCITT-FALSE。
//
// 参数：多项式 0x1021，初值 0xFFFF，输入/输出无反射，异或出 0。
// 帧 CRC（单帧完整性）与文件 CRC（整文件验收）共用本实现，仅覆盖范围不同。
// 本模块硬件无关：不依赖任何 esp_* 头文件，可在宿主机编译单测。
#ifndef BUZZ_CRC16_H
#define BUZZ_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 增量喂入：以既有 crc 值继续计算，支持任意分块。
// 新会话从 BUZZ_CRC16_INIT 开始；帧 CRC 与文件 CRC 均由此函数累计。
uint16_t buzz_crc16_update(uint16_t crc, const uint8_t *data, size_t len);

// 一次性计算整段数据的 CRC。
uint16_t buzz_crc16(const uint8_t *data, size_t len);

// CRC 初值（算法定义，显式命名便于调用点自描述）。
#define BUZZ_CRC16_INIT 0xFFFFu

#ifdef __cplusplus
}
#endif

#endif // BUZZ_CRC16_H
