import BuzzLink
import BuzzLinkCore
import Foundation

/// 弱引用盒：onSend 闭包需要回灌 transport.inbound 时，
/// 解决"闭包创建早于 transport 实例"的循环引用问题。
final class WeakBox<Value: AnyObject>: @unchecked Sendable {
    weak var value: Value?

    init(_ value: Value? = nil) {
        self.value = value
    }
}

/// 线程安全的客户端状态收集器（供 stateUpdates 消费任务使用）。
final class ClientStateCollector: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [BuzzLinkClientState] = []

    var values: [BuzzLinkClientState] {
        lock.withLock { storage }
    }

    func append(_ state: BuzzLinkClientState) {
        lock.withLock { storage.append(state) }
    }
}

/// 等待任务完成并归拢为 Result（便于失败路径断言编排）。
func withTaskResult<Success>(_ task: Task<Success, any Error>) async -> Result<Success, any Error> {
    do {
        return .success(try await task.value)
    } catch {
        return .failure(error)
    }
}

/// 有界轮询直到条件成立或超时（时序纪律：禁固定 sleep）。
/// 用于"await 操作返回后等异步流消费任务收敛到期望记录"的竞态防御：
/// actor 侧完成只保证 yield 已发生，不保证消费任务已 append。
func pollUntil(timeout: TimeInterval, _ predicate: @escaping () async -> Bool) async -> Bool {
    let deadline = ProcessInfo.processInfo.systemUptime + timeout
    while ProcessInfo.processInfo.systemUptime < deadline {
        if await predicate() { return true }
        try? await Task.sleep(for: .milliseconds(10))
    }
    return await predicate()
}


/// 可脚本化 mock 传输：把客户端字节流交给 `onSend` 闭包，由测试自行决定
/// 回包内容/时机（同步回包、延迟回包、不回包模拟超时、发断连状态等）。
///
/// 线程安全：配置在创建时固化（let），可变状态（发送记录）以 NSLock.withLock 保护；
/// 入站/状态经 AsyncStream continuation 产出（线程安全）。
final class MockTransport: BuzzLinkTransport, @unchecked Sendable {

    /// 客户端每次 send 的回调（入参为一帧完整线上字节）
    let onSend: (@Sendable (Data) -> Void)?

    private let lock = NSLock()
    private var sentByteChunks: [Data] = []
    private var isDisconnected = false

    private let stateContinuation: AsyncStream<BuzzLinkTransportState>.Continuation
    private let inboundContinuation: AsyncStream<Data>.Continuation
    private let connectError: (any Error)?
    private let connectStates: [BuzzLinkTransportState]

    let stateStream: AsyncStream<BuzzLinkTransportState>
    let inbound: AsyncStream<Data>

    init(
        onSend: (@Sendable (Data) -> Void)? = nil,
        connectError: (any Error)? = nil,
        connectStates: [BuzzLinkTransportState]? = nil
    ) {
        self.onSend = onSend
        self.connectError = connectError
        // 默认：connecting → ready
        self.connectStates = connectStates ?? [.connecting, .ready]
        let states = AsyncStream<BuzzLinkTransportState>.makeStream()
        self.stateStream = states.stream
        self.stateContinuation = states.continuation
        let inbound = AsyncStream<Data>.makeStream()
        self.inbound = inbound.stream
        self.inboundContinuation = inbound.continuation
    }

    /// 已发送的字节片段（加锁快照）
    var sentChunks: [Data] {
        lock.withLock { sentByteChunks }
    }

    /// 已发送的帧（按整帧解码；测试保证每次 send 恰好一帧）
    var sentFrames: [BuzzLinkFrame] {
        sentChunks.compactMap { try? BuzzLinkFrame.decoded(from: $0) }
    }

    // MARK: - BuzzLinkTransport

    func connect() async throws {
        for state in connectStates {
            stateContinuation.yield(state)
        }
        if let connectError {
            stateContinuation.yield(.disconnected)
            throw connectError
        }
    }

    func disconnect() {
        let wasDisconnected = lock.withLock { () -> Bool in
            let already = isDisconnected
            isDisconnected = true
            return already
        }
        if !wasDisconnected {
            stateContinuation.yield(.disconnected)
        }
    }

    func send(_ bytes: Data) async throws {
        let disconnected = lock.withLock { () -> Bool in
            if !isDisconnected {
                sentByteChunks.append(bytes)
            }
            return isDisconnected
        }
        if disconnected {
            throw BuzzLinkError.disconnected
        }
        onSend?(bytes)
    }

    // MARK: - 测试驱动入口

    /// 喂入入站字节（模拟设备下行）
    func emitInbound(_ bytes: Data) {
        inboundContinuation.yield(bytes)
    }

    /// 逐字节喂入（压力测试 §3.3 字节流重组）
    func emitInboundByteByByte(_ bytes: Data) {
        for byte in bytes {
            inboundContinuation.yield(Data([byte]))
        }
    }

    /// 延迟喂入（模拟网络时序）
    func emitInboundLater(_ bytes: Data, afterMilliseconds: Int) {
        Task { [inboundContinuation] in
            try? await Task.sleep(for: .milliseconds(afterMilliseconds))
            inboundContinuation.yield(bytes)
        }
    }

    func emitState(_ state: BuzzLinkTransportState) {
        if state == .disconnected {
            lock.withLock {
                isDisconnected = true
            }
        }
        stateContinuation.yield(state)
    }

    /// 模拟传输层故障：后续 send 抛 .disconnected，并发出断连状态
    func failSends() {
        lock.withLock {
            isDisconnected = true
        }
        stateContinuation.yield(.disconnected)
    }
}
