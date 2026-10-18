import BuzzLinkCore
import Foundation

/// 帧旁听事件：TX/RX 两侧经 ``BuzzLinkFrameParser`` 重组出的完整帧（含原始线上字节）。
/// `ts` 为系统时钟（``ProcessInfo/processInfo/systemUptime``），展示层自行换算相对时间。
public struct FrameTapEvent: Equatable, Sendable {
    public let ts: TimeInterval
    public let direction: BuzzLinkTestVector.Direction
    public let frame: BuzzLinkFrame
    /// 原始线上字节（含魔数与 CRC；重组帧重新编码等价——CRC 已在解析时校验）
    public let rawBytes: Data

    public init(ts: TimeInterval, direction: BuzzLinkTestVector.Direction, frame: BuzzLinkFrame, rawBytes: Data) {
        self.ts = ts
        self.direction = direction
        self.frame = frame
        self.rawBytes = rawBytes
    }
}

/// Transport 装饰器（tee）：包一层 transport，两侧以 ``BuzzLinkFrameParser`` 重组帧产出
/// ``FrameTapEvent`` 流——协议控制台逐帧旁听的数据源
/// （T7 Phase A；计划 §2 问题 1 结论形态：`DeviceViewModel` transport 工厂处包装，
/// BLE/loopback 全覆盖，不改 ``BuzzLinkClient`` 一行）。
///
/// 线程模型（计划 §6 性能约束）：不进 actor。TX parser 仅由 `send`（Client actor 串行）
/// 触碰，RX parser 仅由转发任务（单任务）触碰，两者互不共享状态；各以 NSLock 防御意外
/// 并发（uncontended，同 `SimEngineTransport` 先例）。热路径成本 = 一次字节 copy +
/// 增量查表 CRC。
///
/// 流的单播约束：AsyncStream 单消费者竞争——本类型是 `base` 的**唯一**消费者，
/// 对外重建 `inbound`/`stateStream` 两条流（转发 = 内部重组 + 外部广播）：
/// Client 消费本类型的流拿到全量字节，事件不丢。断连（转发 state 时观察到
/// `.disconnected`）先 reset 两侧 parser 再广播——真实链路中断连先于后续数据到达，
/// 重连后首帧即新会话边界（半包残留被丢弃）。
public final class FrameTapTransport: BuzzLinkTransport, @unchecked Sendable {

    private let base: any BuzzLinkTransport
    private let eventStream = AsyncStream<FrameTapEvent>.makeStream()
    private let inboundStream = AsyncStream<Data>.makeStream()
    private let stateStreamOut = AsyncStream<BuzzLinkTransportState>.makeStream()
    private let lockTX = NSLock()
    private let lockRX = NSLock()
    private var parserTX = BuzzLinkFrameParser()
    private var parserRX = BuzzLinkFrameParser()
    private var baseTasks: [Task<Void, Never>] = []

    /// 入站字节流（重建流：base 唯一消费者在本类内转发，外部消费不丢数据）。
    public nonisolated var inbound: AsyncStream<Data> { inboundStream.stream }

    /// 状态流（重建流：转发前同步执行 parser reset）。
    public nonisolated var stateStream: AsyncStream<BuzzLinkTransportState> { stateStreamOut.stream }

    /// 旁听事件流（消费方为独立任务；背压由 AsyncStream 缓冲承担——事件产量 = 帧产量，
    /// 39.8 KB/s ≈ 180 帧/s，缓冲裕量充足，控制台消费缓慢不会反压同步路径）。
    public nonisolated var events: AsyncStream<FrameTapEvent> { eventStream.stream }

    public init(wrapping base: any BuzzLinkTransport) {
        self.base = base
        let states = base.stateStream
        let inbound = base.inbound
        baseTasks = [
            Task { [weak self] in
                for await state in states {
                    if state == .disconnected { self?.resetParsers() }
                    self?.stateStreamOut.continuation.yield(state)
                }
            },
            Task { [weak self] in
                for await bytes in inbound {
                    self?.consumeInbound(bytes)
                    self?.inboundStream.continuation.yield(bytes)
                }
            },
        ]
    }

    deinit {
        baseTasks.forEach { $0.cancel() }
    }

    public func connect() async throws {
        // 新会话边界：清空两侧 parser（重连时 inbound 流在真实 transport 上延续——
        // 如 SimEngineTransport 同实例重连——任何残留半包不得带入新会话）。
        // 与 .disconnected 监听处的 reset 互补：双点幂等，不依赖事件到达时序。
        resetParsers()
        try await base.connect()
    }

    public func disconnect() {
        base.disconnect()
    }

    /// 发送前重组出帧并标记 TX 方向（与 Client 同一份 parser 语义：粘包/拆包/校验一致）。
    public func send(_ bytes: Data) async throws {
        drainTX(bytes)
        try await base.send(bytes)
    }

    // MARK: - 私有

    /// TX parser 重组（同步方法：锁在同步上下文使用合法；Swift 6 禁 async 上下文裸 lock）。
    private func drainTX(_ bytes: Data) {
        lockTX.lock()
        parserTX.append(bytes)
        while let frame = parserTX.nextFrame() {
            emit(direction: .tx, frame: frame)
        }
        lockTX.unlock()
    }

    private func consumeInbound(_ bytes: Data) {
        lockRX.lock()
        parserRX.append(bytes)
        while let frame = parserRX.nextFrame() {
            emit(direction: .rx, frame: frame)
        }
        lockRX.unlock()
    }

    private func resetParsers() {
        lockTX.lock(); parserTX.reset(); lockTX.unlock()
        lockRX.lock(); parserRX.reset(); lockRX.unlock()
    }

    private func emit(direction: BuzzLinkTestVector.Direction, frame: BuzzLinkFrame) {
        eventStream.continuation.yield(FrameTapEvent(
            ts: ProcessInfo.processInfo.systemUptime,
            direction: direction,
            frame: frame,
            rawBytes: (try? frame.encoded()) ?? Data()
        ))
    }
}
