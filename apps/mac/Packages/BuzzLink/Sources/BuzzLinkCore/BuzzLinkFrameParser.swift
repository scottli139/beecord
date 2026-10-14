import Foundation

/// 字节流 → 帧重组器（BUZZLINK.md §3.3：接收方必须按字节流重组，
/// magic 同步 + length 取帧，容忍底层任意切分或合并）。
///
/// 用法：收到 GATT 数据就 `append(_:)`，随后循环调 `nextFrame()` 直到返回 nil。
/// 按 §10 的宽容策略，以下帧会被**静默丢弃**并计入 `discardedFrameCount`：
/// 未知 version、未知 type、未知 flags 位、CRC 错误。丢弃后自动重新同步，
/// 不中断后续帧的解析。
public struct BuzzLinkFrameParser: Sendable {

    /// 累计丢弃的帧数（含 CRC 错误与未知 version/type/flags）
    public private(set) var discardedFrameCount = 0

    private var buffer = Data()

    public init() {}

    /// 追加收到的字节（一次 GATT notify/write 的任意切片）
    public mutating func append(_ bytes: Data) {
        buffer.append(bytes)
    }

    /// 尝试取出下一帧。
    /// - Returns: 解析出的帧；数据不足时返回 nil（坏帧在内部丢弃后继续扫描）
    public mutating func nextFrame() -> BuzzLinkFrame? {
        while true {
            guard synchronizeToMagic() else { return nil }

            // 帧头不足：等更多数据
            guard buffer.count >= BuzzLinkConstants.headerLength else { return nil }

            // version 未知：length 语义不可信，丢 1 字节重新同步（§10）
            guard buffer[buffer.startIndex + 2] == BuzzLinkConstants.version else {
                discardBytes(1)
                continue
            }

            let payloadLength = Int(buffer.readLE16(at: buffer.startIndex + 8))
            let frameLength = BuzzLinkConstants.frameOverhead + payloadLength

            // 整帧不足：等更多数据
            guard buffer.count >= frameLength else { return nil }

            do {
                let frame = try BuzzLinkFrame.decoded(from: Data(buffer.prefix(frameLength)))
                // version 合法但 type / flags 未知：忽略该帧，帧边界仍可信（§10）
                guard BuzzLinkFrameType(rawValue: frame.type) != nil,
                      frame.flags & ~BuzzLinkConstants.flagFragment == 0 else {
                    discardBytes(frameLength)
                    continue
                }
                buffer.removeFirst(frameLength)
                return frame
            } catch {
                // CRC 错误或结构错误：payload 内可能藏假 magic，丢 1 字节重扫
                discardBytes(1)
                continue
            }
        }
    }

    /// 重置内部缓冲（如连接断开后重用）
    public mutating func reset() {
        buffer.removeAll(keepingCapacity: true)
    }

    // MARK: - 私有

    /// 把缓冲对齐到下一个 magic；找不到时仅保留可能是半个 magic 的尾字节。
    /// - Returns: 是否已对齐（缓冲以 magic 开头）
    private mutating func synchronizeToMagic() -> Bool {
        var index = buffer.startIndex
        let lastPairIndex = buffer.index(buffer.endIndex, offsetBy: -1, limitedBy: buffer.startIndex) ?? buffer.startIndex
        while index < lastPairIndex {
            let next = buffer.index(after: index)
            if buffer[index] == BuzzLinkConstants.magicByte0, buffer[next] == BuzzLinkConstants.magicByte1 {
                if index > buffer.startIndex {
                    buffer.removeFirst(index - buffer.startIndex)
                }
                return true
            }
            index = next
        }
        // 没找到完整 magic：最后一个字节若是魔数首字节则保留，其余丢弃
        if let last = buffer.last, last == BuzzLinkConstants.magicByte0 {
            buffer = Data([last])
        } else {
            buffer.removeAll(keepingCapacity: true)
        }
        return false
    }

    private mutating func discardBytes(_ count: Int) {
        discardedFrameCount += 1
        buffer.removeFirst(count)
    }
}
