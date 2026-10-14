import Foundation

extension Data {
    /// 以小端序追加 16 位无符号整数
    mutating func appendLE16(_ value: UInt16) {
        append(UInt8(value & 0xFF))
        append(UInt8((value >> 8) & 0xFF))
    }

    /// 以小端序追加 32 位无符号整数
    mutating func appendLE32(_ value: UInt32) {
        append(UInt8(value & 0xFF))
        append(UInt8((value >> 8) & 0xFF))
        append(UInt8((value >> 16) & 0xFF))
        append(UInt8((value >> 24) & 0xFF))
    }

    /// 以小端序读取 16 位无符号整数；调用方保证 `offset + 2 <= count`
    func readLE16(at offset: Int) -> UInt16 {
        UInt16(self[offset]) | (UInt16(self[offset + 1]) << 8)
    }

    /// 以小端序读取 32 位无符号整数；调用方保证 `offset + 4 <= count`
    func readLE32(at offset: Int) -> UInt32 {
        UInt32(self[offset])
            | (UInt32(self[offset + 1]) << 8)
            | (UInt32(self[offset + 2]) << 16)
            | (UInt32(self[offset + 3]) << 24)
    }
}
