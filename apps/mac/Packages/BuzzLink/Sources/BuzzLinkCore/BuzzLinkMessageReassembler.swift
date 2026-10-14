import Foundation

/// 分片消息重组器（BUZZLINK.md §3.3）。
///
/// 非分片帧直接透传；分片帧按 `(type, session, channel)` 键缓存，
/// 拼齐 `fragCount` 片后返回重组出的完整消息帧（FRAG 位清零，payload 为拼接结果）。
/// 容忍分片乱序与重复到达；一致性错误抛 ``BuzzLinkReassemblyError`` 并丢弃该消息的部分状态。
/// 部分消息的超时清理由上层（会话层）负责，本类型不含定时器。
public struct BuzzLinkMessageReassembler: Sendable {

    public init() {}

    /// 喂入一帧。
    /// - Parameter frame: 已通过帧级校验的帧
    /// - Returns: 非分片帧原样返回；分片未齐返回 nil；分片拼齐返回重组帧
    /// - Throws: ``BuzzLinkReassemblyError``
    @discardableResult
    public mutating func accept(_ frame: BuzzLinkFrame) throws -> BuzzLinkFrame? {
        guard frame.isFragment else { return frame }

        guard frame.payload.count >= BuzzLinkConstants.fragmentHeaderLength else {
            throw BuzzLinkReassemblyError.fragmentHeaderTooShort
        }
        let fragIndex = Int(frame.payload.readLE16(at: frame.payload.startIndex))
        let fragCount = Int(frame.payload.readLE16(at: frame.payload.startIndex + 2))
        let data = Data(frame.payload.dropFirst(BuzzLinkConstants.fragmentHeaderLength))

        guard fragCount >= 1 else {
            throw BuzzLinkReassemblyError.invalidFragmentCount
        }
        guard fragIndex < fragCount else {
            throw BuzzLinkReassemblyError.fragmentIndexOutOfRange(index: fragIndex, count: fragCount)
        }

        let key = FragmentKey(type: frame.type, session: frame.session, channel: frame.channel)
        var partial = partials[key] ?? PartialMessage(fragCount: fragCount)

        // 同消息的各分片 fragCount 必须一致
        guard partial.fragCount == fragCount else {
            partials.removeValue(forKey: key)
            throw BuzzLinkReassemblyError.inconsistentFragmentCount
        }

        // 重复片：内容相同幂等忽略，内容冲突报错
        if let existing = partial.chunks[fragIndex] {
            guard existing == data else {
                partials.removeValue(forKey: key)
                throw BuzzLinkReassemblyError.conflictingFragment(index: fragIndex)
            }
            return nil
        }

        partial.receivedBytes += data.count
        guard partial.receivedBytes <= BuzzLinkConstants.maxMessageSize else {
            partials.removeValue(forKey: key)
            throw BuzzLinkReassemblyError.messageTooLarge(partial.receivedBytes)
        }
        partial.chunks[fragIndex] = data

        guard partial.chunks.count == partial.fragCount else {
            partials[key] = partial
            return nil
        }

        // 拼齐：按 fragIndex 顺序拼接
        partials.removeValue(forKey: key)
        var message = Data()
        message.reserveCapacity(partial.receivedBytes)
        for index in 0..<partial.fragCount {
            if let chunk = partial.chunks[index] {
                message.append(chunk)
            }
        }
        var reassembled = frame
        reassembled.flags &= ~BuzzLinkConstants.flagFragment
        reassembled.payload = message
        return reassembled
    }

    /// 丢弃全部部分消息（如连接断开后）
    public mutating func reset() {
        partials.removeAll()
    }

    // MARK: - 私有

    private struct FragmentKey: Hashable, Sendable {
        let type: UInt8
        let session: UInt8
        let channel: UInt16
    }

    private struct PartialMessage: Sendable {
        var fragCount: Int
        var chunks: [Int: Data] = [:]
        var receivedBytes: Int = 0
    }

    private var partials: [FragmentKey: PartialMessage] = [:]
}

/// 分片发送器（BUZZLINK.md §3.3）。
public enum BuzzLinkFragmenter {

    /// 把一条消息切成帧序列。消息不超单帧上限时返回单个非分片帧。
    /// - Parameters:
    ///   - type / session / channel: 帧公共字段
    ///   - message: 完整消息字节（上限 ``BuzzLinkConstants/maxMessageSize``）
    ///   - maxPayload: 单帧 payload 上限，默认按 MTU 247
    /// - Returns: 有序帧序列；分片时所有帧置 FRAG，payload 为 `fragIndex fragCount data…`
    /// - Throws: ``BuzzLinkReassemblyError``
    public static func fragment(
        type: UInt8,
        session: UInt8,
        channel: UInt16,
        message: Data,
        maxPayload: Int = BuzzLinkConstants.maxPayloadPerFrame()
    ) throws -> [BuzzLinkFrame] {
        guard maxPayload > BuzzLinkConstants.fragmentHeaderLength else {
            throw BuzzLinkReassemblyError.invalidMaxPayload(maxPayload)
        }
        guard message.count <= BuzzLinkConstants.maxMessageSize else {
            throw BuzzLinkReassemblyError.messageTooLarge(message.count)
        }
        guard message.count > maxPayload else {
            return [BuzzLinkFrame(type: type, session: session, channel: channel, payload: message)]
        }

        let dataPerFragment = maxPayload - BuzzLinkConstants.fragmentHeaderLength
        let fragCount = (message.count + dataPerFragment - 1) / dataPerFragment
        guard fragCount <= Int(UInt16.max) else {
            throw BuzzLinkReassemblyError.messageTooLarge(message.count)
        }

        var frames: [BuzzLinkFrame] = []
        frames.reserveCapacity(fragCount)
        for index in 0..<fragCount {
            var payload = Data()
            payload.reserveCapacity(BuzzLinkConstants.fragmentHeaderLength + dataPerFragment)
            payload.appendLE16(UInt16(index))
            payload.appendLE16(UInt16(fragCount))
            let start = message.startIndex + index * dataPerFragment
            let end = message.index(start, offsetBy: dataPerFragment, limitedBy: message.endIndex) ?? message.endIndex
            payload.append(message[start..<end])
            frames.append(BuzzLinkFrame(
                type: type,
                session: session,
                flags: BuzzLinkConstants.flagFragment,
                channel: channel,
                payload: payload
            ))
        }
        return frames
    }
}

/// 分片重组/发送错误。
public enum BuzzLinkReassemblyError: Error, Equatable, Sendable {
    /// 分片帧 payload 不足 4 字节头
    case fragmentHeaderTooShort
    /// fragCount 为 0
    case invalidFragmentCount
    /// fragIndex ≥ fragCount
    case fragmentIndexOutOfRange(index: Int, count: Int)
    /// 同消息分片的 fragCount 不一致
    case inconsistentFragmentCount
    /// 同一 fragIndex 收到内容冲突的两片
    case conflictingFragment(index: Int)
    /// 重组消息超出 64 KB 上限
    case messageTooLarge(Int)
    /// 调用方给出的单帧上限连分片头都放不下
    case invalidMaxPayload(Int)
}

extension BuzzLinkReassemblyError: LocalizedError {
    public var errorDescription: String? {
        switch self {
        case .fragmentHeaderTooShort:
            return "分片帧 payload 不足 4 字节分片头"
        case .invalidFragmentCount:
            return "fragCount 必须 ≥ 1"
        case .fragmentIndexOutOfRange(let index, let count):
            return "fragIndex \(index) 超出 fragCount \(count) 范围"
        case .inconsistentFragmentCount:
            return "同一消息的各分片 fragCount 不一致"
        case .conflictingFragment(let index):
            return "fragIndex \(index) 收到内容冲突的重复片"
        case .messageTooLarge(let size):
            return "消息大小 \(size) 超出 64 KB 上限"
        case .invalidMaxPayload(let value):
            return "单帧 payload 上限 \(value) 无效（必须大于分片头 4 字节）"
        }
    }
}
