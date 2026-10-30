import BuzzLink
import BuzzLinkCore
import BuzzLinkSimCore
import Foundation

/// 进程内集成传输：把 BuzzLinkClient 的字节流直接 pipe 进 `SimEngine`（对端），
/// 推流按拉取模型由后台泵循环驱动（等同 SimDeviceModel 的推流节奏壳）。
///
/// 与真实链路等价的点：客户端字节流经帧解析/分片重组后才喂引擎；引擎输出经编码
/// （施加 corruptCRC 故障）后按小块切片回灌客户端入站，压测 §3.3 字节流重组。
public final class SimEngineTransport: BuzzLinkTransport, @unchecked Sendable {

    public let stateStream: AsyncStream<BuzzLinkTransportState>
    public let inbound: AsyncStream<Data>

    /// 引擎与重组上下文的互斥访问锁（send 在客户端 actor、泵在后台任务）
    private let lock = NSLock()
    private var engine: SimEngine
    private var parser = BuzzLinkFrameParser()
    private var reassembler = BuzzLinkMessageReassembler()
    private var pumpTask: Task<Void, Never>?
    private var isDisconnected = true

    private let stateContinuation: AsyncStream<BuzzLinkTransportState>.Continuation
    private let inboundContinuation: AsyncStream<Data>.Continuation
    private let now: @Sendable () -> TimeInterval
    private let pumpIntervalMilliseconds: Int
    /// 推流限速（字节/秒；nil = 不限），模拟真实链路节奏
    private let throttleBytesPerSecond: Int?
    /// 入站消息帧按 type 的计数（锁内维护，测试断言用）
    private var frameTypeCounts: [UInt8: Int] = [:]

    /// 最后接受的 TIME_SET（v0.2 §8.4；透传引擎状态，测试断言用）
    public var lastAcceptedTimeSet: (epochMs: Int64, utcOffsetMin: Int)? {
        lock.withLock { engine.lastAcceptedTimeSet }
    }

    /// 链路切片尺寸循环（模拟 notify/socket 任意切分）
    private static let splitPattern: [Int] = [251, 13, 1_024, 2]

    public init(
        engine: SimEngine,
        now: @Sendable @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime },
        pumpIntervalMilliseconds: Int = 2,
        throttleBytesPerSecond: Int? = nil
    ) {
        self.engine = engine
        self.now = now
        self.pumpIntervalMilliseconds = pumpIntervalMilliseconds
        self.throttleBytesPerSecond = throttleBytesPerSecond
        let states = AsyncStream<BuzzLinkTransportState>.makeStream()
        self.stateStream = states.stream
        self.stateContinuation = states.continuation
        let inbound = AsyncStream<Data>.makeStream()
        self.inbound = inbound.stream
        self.inboundContinuation = inbound.continuation
    }

    // MARK: - 测试观测与编排

    /// 入站消息帧按 type 的计数（锁内快照；区分帧类型的时序无关断言用）
    public var receivedFrameTypeCounts: [UInt8: Int] {
        lock.withLock { frameTypeCounts }
    }

    /// 运行时更新故障注入配置（测试编排用）
    public func updateFaults(_ mutate: (inout SimFaults) -> Void) {
        lock.withLock { mutate(&engine.faults) }
    }

    // MARK: - BuzzLinkTransport

    public func connect() async throws {
        lock.withLock {
            parser = BuzzLinkFrameParser()
            reassembler = BuzzLinkMessageReassembler()
            isDisconnected = false
        }
        stateContinuation.yield(.connecting)
        stateContinuation.yield(.ready)
        // §6.1：连接建立即主动推 INFO（session=0x00）
        let infoPush = lock.withLock { engine.makeInfoPush() }
        deliver(infoPush)
        pumpTask = Task { [weak self] in
            while !Task.isCancelled {
                self?.pumpChunks()
                try? await Task.sleep(for: .milliseconds(self?.pumpIntervalMilliseconds ?? 2))
            }
        }
    }

    public func disconnect() {
        pumpTask?.cancel()
        lock.withLock {
            isDisconnected = true
            engine.reset()
        }
        stateContinuation.yield(.disconnected)
    }

    public func send(_ bytes: Data) async throws {
        let result = lock.withLock { () -> (outputs: [SimOutboundFrame], disconnected: Bool) in
            guard !isDisconnected else { return ([], true) }
            parser.append(bytes)
            var collected: [SimOutboundFrame] = []
            while let frame = parser.nextFrame() {
                guard let message = try? reassembler.accept(frame) else { continue }
                frameTypeCounts[message.type, default: 0] += 1
                collected.append(contentsOf: engine.handle(message, now: now()))
            }
            return (collected, false)
        }
        if result.disconnected {
            throw BuzzLinkError.disconnected
        }
        deliver(result.outputs)
    }

    // MARK: - 私有：推流泵与投递

    /// 泵：拉取引擎待推块（丢块/静默故障在此自然生效）
    private func pumpChunks() {
        var outputs: [SimOutboundFrame] = []
        lock.lock()
        if !isDisconnected {
            while let output = engine.nextChunk(now: now()) {
                outputs.append(output)
            }
        }
        lock.unlock()
        deliver(outputs)
    }

    /// 引擎输出 → 编码（施加 corruptCRC）→ 切片回灌客户端入站
    private func deliver(_ outputs: [SimOutboundFrame]) {
        for output in outputs {
            guard var bytes = try? output.frame.encoded() else { continue }
            if output.corruptCRC, !bytes.isEmpty {
                let last = bytes.index(before: bytes.endIndex)
                bytes[last] ^= 0xFF
            }
            emitSplit(bytes)
            if let rate = throttleBytesPerSecond, rate > 0 {
                Thread.sleep(forTimeInterval: Double(bytes.count) / Double(rate))
            }
        }
    }

    /// 按固定模式切片入站（§3.3 客户端必须容忍任意切分/合并）
    private func emitSplit(_ bytes: Data) {
        var offset = 0
        var patternIndex = 0
        while offset < bytes.count {
            let size = Self.splitPattern[patternIndex % Self.splitPattern.count]
            patternIndex += 1
            let end = min(offset + size, bytes.count)
            inboundContinuation.yield(bytes.subdata(in: offset..<end))
            offset = end
        }
    }
}
