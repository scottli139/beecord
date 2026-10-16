import BuzzLinkCore
import BuzzLinkSimCore
import Foundation
import Network
import os

/// 设备流 UITest 的进程内设备源（T5 阶段 D）：裁剪自 BuzzLinkSim 的
/// SimLoopbackServer + SimDeviceModel loopback 部分——NWListener 监听
/// 127.0.0.1:BuzzLinkConstants.loopbackPort，承载与 BLE 空口同构的 BuzzLink
/// 字节流。全部可变状态限定于内部串行队列；bind 失败（本机 Sim app 占用端口）
/// 时 start() 抛出，由用例 XCTSkip。
final class UITestSimServer: @unchecked Sendable {

    enum ServerError: Error, LocalizedError {
        case bindFailed(String)

        var errorDescription: String? {
            switch self {
            case .bindFailed(let message):
                "UITestSimServer 监听失败：\(message)（本机 Sim app 可能正在占用 40404 端口）"
            }
        }
    }

    /// 逐连接入站字节流重组上下文（帧重组 + 分片重组）
    private typealias Ingress = (parser: BuzzLinkFrameParser, reassembler: BuzzLinkMessageReassembler)

    private let queue = DispatchQueue(label: "com.beeuc.beecord.uitest-sim")
    private let logger = Logger(subsystem: "com.beeuc.beecord.studio.uitests", category: "simserver")
    private var engine: SimEngine
    private var listener: NWListener?
    private var connections: [ObjectIdentifier: NWConnection] = [:]
    private var ingress: [ObjectIdentifier: Ingress] = [:]
    private var isRunning = false
    /// 推流泵单链存活标记（stop 后旧链自然退出，防止并发链叠加令限速失效）
    private var pumpChainAlive = false

    init(engine: SimEngine) {
        self.engine = engine
    }

    deinit {
        stop()
    }

    /// 启动监听（幂等）；等待 NWListener 进入 `.ready` 才返回——bind 失败（端口占用等）
    /// 经状态回调异步到达，此处收敛为 ``ServerError/bindFailed`` 抛出，由用例 XCTSkip。
    /// 调用方线程有界等待；状态回调跑在内部串行队列上，无死锁。
    func start() throws {
        let pending = try queue.sync { () throws -> PendingReady? in
            guard listener == nil else { return nil }
            let parameters = NWParameters.tcp
            parameters.requiredInterfaceType = .loopback
            guard let port = NWEndpoint.Port(rawValue: BuzzLinkConstants.loopbackPort) else {
                throw ServerError.bindFailed("端口非法：\(BuzzLinkConstants.loopbackPort)")
            }
            let newListener: NWListener
            do {
                newListener = try NWListener(using: parameters, on: port)
            } catch {
                throw ServerError.bindFailed(error.localizedDescription)
            }
            let pending = PendingReady()
            newListener.newConnectionHandler = { [weak self] connection in
                self?.accept(connection)
            }
            newListener.stateUpdateHandler = { [weak self] state in
                switch state {
                case .ready:
                    pending.resolve(result: .success(()))
                case .failed(let error):
                    // ready 后才失败的为运行期故障：仅记录（启动路径已返回）
                    if !pending.resolve(result: .failure(error)) {
                        self?.logger.error("loopback 监听运行期失败：\(error.localizedDescription)")
                    }
                default:
                    break
                }
            }
            listener = newListener
            isRunning = true
            startPumpIfNeeded()
            newListener.start(queue: queue)
            return pending
        }
        guard let pending else { return }
        if case .timedOut = pending.semaphore.wait(timeout: .now() + 3) {
            abandonPendingListener()
            throw ServerError.bindFailed("等待监听就绪超时")
        }
        if case .failure(let error) = pending.result {
            abandonPendingListener()
            throw ServerError.bindFailed(error.localizedDescription)
        }
    }

    /// 启动失败回收：清引用并取消（回调经串行队列，与 start 互斥）
    private func abandonPendingListener() {
        queue.sync {
            listener?.cancel()
            listener = nil
            isRunning = false
        }
    }

    /// 停止监听并断开全部连接（重复调用幂等）。
    func stop() {
        queue.sync {
            isRunning = false
            listener?.cancel()
            listener = nil
            for connection in connections.values {
                connection.cancel()
            }
            connections.removeAll()
            ingress.removeAll()
        }
    }

    // MARK: - 私有：连接管理（串行队列上下文）

    private func accept(_ connection: NWConnection) {
        let id = ObjectIdentifier(connection)
        connections[id] = connection
        connection.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .failed, .cancelled:
                self.connections.removeValue(forKey: id)
                self.ingress.removeValue(forKey: id)
                // 客户端全部断开：重置会话（§7：未完成命令作废）
                if self.connections.isEmpty {
                    self.engine.reset()
                }
            default:
                break
            }
        }
        connection.start(queue: queue)
        receiveNext(connection, id: id)
        // §6.1：连接建立即主动推 INFO（session=0x00）
        send(engine.makeInfoPush())
    }

    private func receiveNext(_ connection: NWConnection, id: ObjectIdentifier) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 65_536) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            if let data, !data.isEmpty {
                self.receive(id, bytes: data)
            }
            if isComplete || error != nil {
                connection.cancel()
                self.connections.removeValue(forKey: id)
                self.ingress.removeValue(forKey: id)
                if self.connections.isEmpty {
                    self.engine.reset()
                }
                return
            }
            self.receiveNext(connection, id: id)
        }
    }

    /// 入站：字节流 → 帧 → 分片重组 → 引擎 → 应答广播
    private func receive(_ id: ObjectIdentifier, bytes: Data) {
        var context = ingress[id] ?? (parser: BuzzLinkFrameParser(), reassembler: BuzzLinkMessageReassembler())
        context.parser.append(bytes)
        while let frame = context.parser.nextFrame() {
            do {
                if let message = try context.reassembler.accept(frame) {
                    send(engine.handle(message, now: Self.now()))
                }
            } catch {
                logger.error("分片重组失败：\(error.localizedDescription)")
            }
        }
        ingress[id] = context
        // 推流由单一泵链驱动（startPumpIfNeeded），此处不再调度，防并发链叠加
    }

    /// 广播给全部已连接客户端（设备语义：等同 BLE notify）
    private func send(_ outputs: [SimOutboundFrame]) {
        guard !outputs.isEmpty else { return }
        for output in outputs {
            do {
                var bytes = try output.frame.encoded()
                if output.corruptCRC {
                    // 故障注入：编码后翻转 CRC 字段末字节
                    let lastIndex = bytes.index(before: bytes.endIndex)
                    bytes[lastIndex] ^= 0xFF
                }
                for connection in connections.values {
                    connection.send(content: bytes, completion: .idempotent)
                }
            } catch {
                logger.error("帧编码失败：\(error.localizedDescription)")
            }
        }
    }

    // MARK: - 私有：推流泵（拉取模型；单链自调度，限速按 payload/rate 延迟下一拍）

    /// 启动推流泵主链（幂等：重复调用不产生并发链）
    private func startPumpIfNeeded() {
        guard isRunning, pumpChainAlive == false else { return }
        pumpChainAlive = true
        queue.asyncAfter(deadline: .now() + 0.002) { [weak self] in
            self?.pump()
        }
    }

    private func pump() {
        guard isRunning else {
            pumpChainAlive = false
            return
        }
        var nextDelay: TimeInterval = 0.002
        if let output = engine.nextChunk(now: Self.now()) {
            send([output])
            if let rate = engine.faults.throttleBytesPerSecond, rate > 0 {
                nextDelay = Double(output.frame.payload.count) / Double(rate)
            }
        }
        queue.asyncAfter(deadline: .now() + nextDelay) { [weak self] in
            self?.pump()
        }
    }

    private static func now() -> TimeInterval {
        ProcessInfo.processInfo.systemUptime
    }

    /// start() 的就绪等待盒：状态回调（串行队列）resolve 一次并 signal；
    /// 调用方线程 wait 后读 result（semaphore 提供 happens-before）。
    private final class PendingReady: @unchecked Sendable {
        let semaphore = DispatchSemaphore(value: 0)
        private(set) var result: Result<Void, NWError>?

        /// - Returns: 是否本次 resolve 生效（false = 已 resolve 过，属运行期故障）
        @discardableResult
        func resolve(result: Result<Void, NWError>) -> Bool {
            guard self.result == nil else { return false }
            self.result = result
            semaphore.signal()
            return true
        }
    }
}
