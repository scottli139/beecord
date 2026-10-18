import Foundation

/// 控制台帧日志环形缓冲（T7 Phase A；studio-tech-design §5.4"环形缓冲最近 5000 帧"）。
///
/// 纯值语义、线程安全由调用方保证（app 层 @MainActor 串行访问）；5000 条 × ~300B ≈ 1.5MB。
/// 行 ID 单调分配、淘汰不重用（展开选中集稳定键）；时间戳为相对首条记录的**毫秒**
/// （与 ``BuzzLinkTestVector/Entry/ts`` schema 对齐）。
public struct FrameLogBuffer: Sendable {

    public typealias Direction = BuzzLinkTestVector.Direction

    /// 一条帧记录
    public struct Entry: Equatable, Sendable {
        /// 行 ID（单调递增，淘汰不重用）
        public let rowID: Int
        /// 相对会话起点的时间戳（毫秒）
        public let ts: TimeInterval
        public let direction: Direction
        public let frame: BuzzLinkFrame
        /// 展示注释（可选）
        public var comment: String?
    }

    /// 过滤谓词（方向 + type 多选并集 + 文本子串，三者 AND；空/nil 为不限）
    public struct Filter: Equatable, Sendable {
        /// nil = 全部方向
        public var direction: Direction?
        /// 空集 = 全部 type（多选并集）
        public var types: Set<UInt8>
        /// payload hex / 注释 / type 名子串（小写包含匹配）；空 = 不限
        public var text: String

        public init(direction: Direction? = nil, types: Set<UInt8> = [], text: String = "") {
            self.direction = direction
            self.types = types
            self.text = text
        }
    }

    /// 环形容量（淘汰最旧）
    public static let capacity = 5000

    private var storage: [Entry] = []
    private var nextRowID = 0
    private var t0: TimeInterval?
    private let clock: @Sendable () -> TimeInterval

    public init(clock: @escaping @Sendable () -> TimeInterval = { ProcessInfo.processInfo.systemUptime }) {
        self.clock = clock
    }

    public var count: Int { storage.count }
    public var isEmpty: Bool { storage.isEmpty }
    /// 当前快照（导出/测试用；导出含未过滤的全部原始帧——向量回放需完整会话）
    public var entries: [Entry] { storage }
    /// 下一个将被分配的行 ID（测试断言单调性用）
    public var pendingRowID: Int { nextRowID }

    /// 追加一条记录；超容量淘汰最旧，行 ID 不重用。
    public mutating func append(frame: BuzzLinkFrame, direction: Direction, comment: String? = nil) {
        if t0 == nil { t0 = clock() }
        storage.append(Entry(
            rowID: nextRowID,
            ts: (clock() - (t0 ?? clock())) * 1000,
            direction: direction,
            frame: frame,
            comment: comment
        ))
        nextRowID += 1
        if storage.count > Self.capacity {
            storage.removeFirst(storage.count - Self.capacity)
        }
    }

    /// 清空（连接断开/手动清除）；会话起点与行 ID 序列保留（ID 跨会话仍单调）。
    public mutating func clear() {
        storage.removeAll()
        t0 = nil
    }

    /// 过滤后的展示序列（顺序 = 追加顺序）。
    public func filtered(_ filter: Filter) -> [Entry] {
        storage.filter { entry in
            if let direction = filter.direction, entry.direction != direction { return false }
            if !filter.types.isEmpty && !filter.types.contains(entry.frame.type) { return false }
            let text = filter.text.trimmingCharacters(in: .whitespaces)
            if !text.isEmpty {
                let haystack = (
                    Self.typeName(entry.frame.type) + " "
                        + hexString(entry.frame.payload) + " "
                        + (entry.comment ?? "")
                ).lowercased()
                guard haystack.contains(text.lowercased()) else { return false }
            }
            return true
        }
    }

    /// 导出为 `buzzlink-vector/1` 测试向量（schema 复用 ``BuzzLinkTestVector``；
    /// comment 补 type 名，导出为全量原始帧——回放需完整会话，非过滤结果）。
    public func exportVector(comment: String? = nil) -> BuzzLinkTestVector {
        BuzzLinkTestVector(
            comment: comment ?? "Beecord Studio 协议控制台导出",
            entries: storage.map { entry in
                BuzzLinkTestVector.Entry(
                    ts: entry.ts,
                    direction: entry.direction,
                    type: entry.frame.type,
                    session: entry.frame.session,
                    channel: entry.frame.channel,
                    payloadHex: hexString(entry.frame.payload),
                    comment: entry.comment.map { "\(Self.typeName(entry.frame.type)) · \($0)" }
                        ?? Self.typeName(entry.frame.type)
                )
            }
        )
    }

    /// type 显示名（BUZZLINK.md §5.1 命令集全表；未知值回退十六进制）。
    public static func typeName(_ type: UInt8) -> String {
        switch type {
        case 0x01: return "PING"
        case 0x02: return "PONG"
        case 0x03: return "INFO"
        case 0x04: return "INFO_GET"
        case 0x10: return "LIST_REQ"
        case 0x11: return "LIST_RESP"
        case 0x20: return "FILE_INFO_REQ"
        case 0x21: return "FILE_INFO_RESP"
        case 0x30: return "DL_BEGIN_REQ"
        case 0x31: return "DL_BEGIN_RESP"
        case 0x32: return "DL_CHUNK"
        case 0x33: return "DL_END_REQ"
        case 0x34: return "DL_END_RESP"
        case 0x35: return "DL_ABORT"
        case 0x40: return "FILE_DELETE_REQ"
        case 0x41: return "FILE_DELETE_RESP"
        case 0x7F: return "ERROR"
        default: return String(format: "0x%02X", type)
        }
    }

    private func hexString(_ data: Data) -> String {
        data.map { String(format: "%02x", $0) }.joined()
    }
}
