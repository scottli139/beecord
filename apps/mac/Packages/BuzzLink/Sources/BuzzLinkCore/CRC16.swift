import Foundation

/// CRC-16/CCITT-FALSE（BUZZLINK.md §3.2）：多项式 0x1021，初值 0xFFFF，
/// 无输入/输出反射，异或出 0。
///
/// 自测向量：`crc16("123456789") == 0x29B1`，`crc16("Beecord") == 0x5D1E`。
public enum CRC16 {

    /// 计算字节的 CRC-16/CCITT-FALSE。
    /// - Parameters:
    ///   - bytes: 待校验字节（`Data`、切片或数组均可）
    ///   - seed: 链式计算时的上一轮 CRC 值；单次计算用默认 0xFFFF
    /// - Returns: CRC 值
    public static func ccittFalse<S: Sequence>(_ bytes: S, updating seed: UInt16 = 0xFFFF) -> UInt16 where S.Element == UInt8 {
        var crc = seed
        for byte in bytes {
            let index = Int(UInt8(truncatingIfNeeded: crc >> 8) ^ byte)
            crc = (crc &<< 8) ^ table[index]
        }
        return crc
    }

    private static let table: [UInt16] = (0...255).map { byte in
        var crc = UInt16(byte) &<< 8
        for _ in 0..<8 {
            crc = (crc & 0x8000) != 0 ? (crc &<< 1) ^ 0x1021 : crc &<< 1
        }
        return crc
    }
}
