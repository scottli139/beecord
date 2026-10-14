import Foundation

/// BuzzLink 应用层帧（BUZZLINK.md §3.1）。
///
/// 值语义。`encoded()` 产出含魔数与 CRC 的完整帧字节；
/// `decoded(from:)` 要求输入恰好为一帧（不多不少），逐字节流重组见
/// ``BuzzLinkFrameParser``。
public struct BuzzLinkFrame: Equatable, Sendable {

    public var type: UInt8
    public var session: UInt8
    public var flags: UInt8
    public var channel: UInt16
    public var payload: Data

    public init(type: UInt8, session: UInt8, flags: UInt8 = 0, channel: UInt16, payload: Data = Data()) {
        self.type = type
        self.session = session
        self.flags = flags
        self.channel = channel
        self.payload = payload
    }

    /// 是否带 FRAG 分片标记（§3.3）
    public var isFragment: Bool {
        flags & BuzzLinkConstants.flagFragment != 0
    }

    /// 编码为完整帧字节（魔数 + 帧头 + payload + CRC16）。
    /// - Throws: ``BuzzLinkFrameError/payloadTooLarge(_:)``——payload 超出 length 字段可表达范围
    /// - Returns: 长度为 `12 + payload.count` 的完整帧
    public func encoded() throws -> Data {
        guard payload.count <= Int(UInt16.max) else {
            throw BuzzLinkFrameError.payloadTooLarge(payload.count)
        }
        var out = Data()
        out.reserveCapacity(BuzzLinkConstants.frameOverhead + payload.count)
        out.append(BuzzLinkConstants.magicByte0)
        out.append(BuzzLinkConstants.magicByte1)
        out.append(BuzzLinkConstants.version)
        out.append(type)
        out.append(session)
        out.append(flags)
        out.appendLE16(channel)
        out.appendLE16(UInt16(payload.count))
        out.append(payload)
        let crc = CRC16.ccittFalse(out.suffix(from: 2))
        out.appendLE16(crc)
        return out
    }

    /// 从恰好一帧的字节解码并校验 CRC。
    /// - Parameter bytes: 完整帧字节，长度必须等于 `12 + length` 字段值
    /// - Throws: ``BuzzLinkFrameError``
    public static func decoded(from bytes: Data) throws -> BuzzLinkFrame {
        guard bytes.count >= BuzzLinkConstants.frameOverhead else {
            throw BuzzLinkFrameError.truncatedFrame
        }
        guard bytes[bytes.startIndex] == BuzzLinkConstants.magicByte0,
              bytes[bytes.startIndex + 1] == BuzzLinkConstants.magicByte1 else {
            throw BuzzLinkFrameError.badMagic
        }
        let payloadLength = Int(bytes.readLE16(at: bytes.startIndex + 8))
        guard bytes.count == BuzzLinkConstants.frameOverhead + payloadLength else {
            throw BuzzLinkFrameError.lengthMismatch(
                declared: payloadLength,
                actual: bytes.count - BuzzLinkConstants.frameOverhead
            )
        }
        let bodyEnd = bytes.startIndex + BuzzLinkConstants.headerLength + payloadLength
        let expectedCRC = bytes.readLE16(at: bodyEnd)
        let actualCRC = CRC16.ccittFalse(bytes[(bytes.startIndex + 2)..<bodyEnd])
        guard expectedCRC == actualCRC else {
            throw BuzzLinkFrameError.crcMismatch(expected: expectedCRC, actual: actualCRC)
        }
        return BuzzLinkFrame(
            type: bytes[bytes.startIndex + 3],
            session: bytes[bytes.startIndex + 4],
            flags: bytes[bytes.startIndex + 5],
            channel: bytes.readLE16(at: bytes.startIndex + 6),
            payload: Data(bytes[(bytes.startIndex + BuzzLinkConstants.headerLength)..<bodyEnd])
        )
    }
}

/// 帧编解码错误。
public enum BuzzLinkFrameError: Error, Equatable, Sendable {
    /// 魔数不是 `BE EC`
    case badMagic
    /// 字节数不足一帧最小长度
    case truncatedFrame
    /// length 字段声明值与实际 payload 字节数不一致
    case lengthMismatch(declared: Int, actual: Int)
    /// CRC 校验失败
    case crcMismatch(expected: UInt16, actual: UInt16)
    /// payload 超出 length 字段可表达范围（65535）
    case payloadTooLarge(Int)
}

extension BuzzLinkFrameError: LocalizedError {
    public var errorDescription: String? {
        switch self {
        case .badMagic:
            return "帧魔数不匹配"
        case .truncatedFrame:
            return "帧字节数不足最小长度"
        case .lengthMismatch(let declared, let actual):
            return "帧 length 字段（\(declared)）与实际 payload 字节数（\(actual)）不一致"
        case .crcMismatch(let expected, let actual):
            return String(format: "帧 CRC 校验失败：期望 0x%04X，实际 0x%04X", expected, actual)
        case .payloadTooLarge(let count):
            return "payload 长度 \(count) 超出可编码范围"
        }
    }
}
