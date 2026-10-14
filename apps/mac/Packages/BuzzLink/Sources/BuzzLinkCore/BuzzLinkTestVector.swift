import Foundation

/// 会话测试向量（TESTING.md §3）：JSON 格式，一文件一会话。
///
/// 同一份 JSON 供 Mac 侧 XCTest 与固件侧 C 表单测消费（转换脚本在 `tools/`），
/// 也是协议控制台导出的格式。字段示例：
///
/// ```json
/// {
///   "schema": "buzzlink-vector/1",
///   "comment": "基础会话",
///   "entries": [
///     { "ts": 0.0, "direction": "tx", "type": "0x01", "channel": "0x0000",
///       "session": "0x2A", "payloadHex": "", "comment": "PING" }
///   ]
/// }
/// ```
public struct BuzzLinkTestVector: Equatable, Sendable {

    /// 当前支持的 schema 版本
    public static let currentSchema = "buzzlink-vector/1"

    public var schema: String
    public var comment: String?
    public var entries: [Entry]

    public init(schema: String = currentSchema, comment: String? = nil, entries: [Entry]) {
        self.schema = schema
        self.comment = comment
        self.entries = entries
    }
}

extension BuzzLinkTestVector {

    /// 方向（以 Central/客户端为视角）
    public enum Direction: String, Codable, Sendable {
        /// Central → Device（客户端发出）
        case tx
        /// Device → Central（设备发出）
        case rx
    }

    /// 一条消息记录
    public struct Entry: Equatable, Sendable {
        /// 相对会话起点的时间戳（毫秒），可选
        public var ts: Double?
        public var direction: Direction
        public var type: UInt8
        /// 会话号，可选；缺省时由回放方分配
        public var session: UInt8?
        public var channel: UInt16
        /// payload 的 hex 编码（无空格；解析容忍空格与大小写）
        public var payloadHex: String
        public var comment: String?

        public init(
            ts: Double? = nil,
            direction: Direction,
            type: UInt8,
            session: UInt8? = nil,
            channel: UInt16,
            payloadHex: String,
            comment: String? = nil
        ) {
            self.ts = ts
            self.direction = direction
            self.type = type
            self.session = session
            self.channel = channel
            self.payloadHex = payloadHex
            self.comment = comment
        }

        /// 解析 `payloadHex` 为字节。
        /// - Throws: ``BuzzLinkVectorError/malformedHex(_:)``——含非 hex 字符或长度为奇数
        public func decodedPayload() throws -> Data {
            let stripped = payloadHex.filter { !$0.isWhitespace }
            guard stripped.count % 2 == 0, stripped.allSatisfy(\.isHexDigit) else {
                throw BuzzLinkVectorError.malformedHex(payloadHex)
            }
            var data = Data()
            data.reserveCapacity(stripped.count / 2)
            var index = stripped.startIndex
            while index < stripped.endIndex {
                let next = stripped.index(index, offsetBy: 2)
                if let byte = UInt8(stripped[index..<next], radix: 16) {
                    data.append(byte)
                }
                index = next
            }
            return data
        }

        /// 由条目构造帧；`session` 缺省时使用 `defaultSession`。
        /// - Throws: ``BuzzLinkVectorError/malformedHex(_:)``
        public func makeFrame(defaultSession: UInt8 = 0) throws -> BuzzLinkFrame {
            BuzzLinkFrame(
                type: type,
                session: session ?? defaultSession,
                channel: channel,
                payload: try decodedPayload()
            )
        }
    }
}

// MARK: - Codable

extension BuzzLinkTestVector: Codable {

    private enum CodingKeys: String, CodingKey {
        case schema, comment, entries
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        let schema = try container.decode(String.self, forKey: .schema)
        guard schema == Self.currentSchema else {
            throw BuzzLinkVectorError.unsupportedSchema(schema)
        }
        self.schema = schema
        self.comment = try container.decodeIfPresent(String.self, forKey: .comment)
        self.entries = try container.decode([Entry].self, forKey: .entries)
    }
}

extension BuzzLinkTestVector.Entry: Codable {

    private enum CodingKeys: String, CodingKey {
        case ts, direction, type, session, channel, payloadHex, comment
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        self.ts = try container.decodeIfPresent(Double.self, forKey: .ts)
        self.direction = try container.decode(BuzzLinkTestVector.Direction.self, forKey: .direction)
        self.type = try Self.decodeHexInteger(container, key: .type)
        self.session = try Self.decodeOptionalHexInteger(container, key: .session)
        self.channel = try Self.decodeHexInteger(container, key: .channel)
        self.payloadHex = try container.decode(String.self, forKey: .payloadHex)
        self.comment = try container.decodeIfPresent(String.self, forKey: .comment)
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encodeIfPresent(ts, forKey: .ts)
        try container.encode(direction, forKey: .direction)
        try container.encode(String(format: "0x%02X", type), forKey: .type)
        try container.encodeIfPresent(session.map { String(format: "0x%02X", $0) }, forKey: .session)
        try container.encode(String(format: "0x%04X", channel), forKey: .channel)
        try container.encode(payloadHex, forKey: .payloadHex)
        try container.encodeIfPresent(comment, forKey: .comment)
    }

    /// type/channel/session 宽容解析：接受 `"0x30"`、`"30"`（hex 字符串）或 JSON 数字
    private static func decodeHexInteger<T: FixedWidthInteger & Decodable>(
        _ container: KeyedDecodingContainer<CodingKeys>,
        key: CodingKeys
    ) throws -> T {
        if let string = try? container.decode(String.self, forKey: key) {
            var text = string.trimmingCharacters(in: .whitespaces)
            if text.hasPrefix("0x") || text.hasPrefix("0X") {
                text = String(text.dropFirst(2))
            }
            guard let value = T(text, radix: 16) else {
                throw BuzzLinkVectorError.malformedHex(string)
            }
            return value
        }
        return try container.decode(T.self, forKey: key)
    }

    private static func decodeOptionalHexInteger<T: FixedWidthInteger & Decodable>(
        _ container: KeyedDecodingContainer<CodingKeys>,
        key: CodingKeys
    ) throws -> T? {
        guard container.contains(key), try !container.decodeNil(forKey: key) else {
            return nil
        }
        return try decodeHexInteger(container, key: key)
    }
}

/// 测试向量错误。
public enum BuzzLinkVectorError: Error, Equatable, Sendable {
    /// schema 版本不支持
    case unsupportedSchema(String)
    /// hex 字段非法
    case malformedHex(String)
}

extension BuzzLinkVectorError: LocalizedError {
    public var errorDescription: String? {
        switch self {
        case .unsupportedSchema(let schema):
            return "不支持的向量 schema：\(schema)"
        case .malformedHex(let hex):
            return "非法的 hex 字段：\(hex)"
        }
    }
}
