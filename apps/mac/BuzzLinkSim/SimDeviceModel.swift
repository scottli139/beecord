import BuzzLinkCore
import BuzzLinkSimCore
import CoreBluetooth
import Foundation
import os

/// Sim 日志条目（UI 日志区，环形缓冲）。
struct SimLogEntry: Identifiable, Equatable {
    enum Direction: String {
        case inbound = "→"   // Central → Device
        case outbound = "←"  // Device → Central
        case system = "·"
    }

    let id = UUID()
    let time = Date()
    let direction: Direction
    let text: String
}

/// BuzzLinkSim 应用模型：持有 SimEngine（协议逻辑），驱动 SimPeripheral（GATT IO），
/// 承担字节流重组、推流节奏（限速/静默）、出站故障施加与 UI 状态。
@MainActor @Observable
final class SimDeviceModel {

    // MARK: - 对外状态

    /// 协议引擎（UI 仅允许改 `profile` 与 `faults`）
    var engine: SimEngine
    /// 广播开关（默认开：启动即可被 Studio 发现）
    private(set) var advertisingWanted = true
    /// Loopback 开关（默认开：同机开发通道，TESTING.md §4）
    private(set) var loopbackWanted = true
    /// Loopback 已连接客户端数
    private(set) var loopbackClientCount = 0
    private(set) var bluetoothStateText = "未知"
    private(set) var subscribedCentralCount = 0
    /// 推流进度描述（"REC_… 12/100"）；nil = 空闲
    private(set) var streamProgress: String?
    /// 静默故障生效中（模拟断连）
    private(set) var isSilencedNow = false
    private(set) var logEntries: [SimLogEntry] = []

    // MARK: - 私有

    private let logger = Logger(subsystem: "com.beeuc.beecord.sim", category: "model")
    private let peripheral = SimPeripheral()
    private let loopbackServer = SimLoopbackServer()
    /// 入站链路标识：每条链路独立的字节流重组上下文
    private enum IngressSource: Hashable {
        case ble
        case loopback(ObjectIdentifier)
    }
    private typealias IngressContext = (parser: BuzzLinkFrameParser, reassembler: BuzzLinkMessageReassembler)
    private var ingressContexts: [IngressSource: IngressContext] = [:]
    private var streamTask: Task<Void, Never>?
    private var started = false

    private static let logLimit = 200

    init(engine: SimEngine = SimEngine(files: SimDeviceModel.loadBundledFiles())) {
        self.engine = engine
    }

    /// 单调时钟（秒），注入引擎的 now
    private static func now() -> TimeInterval {
        ProcessInfo.processInfo.systemUptime
    }

    // MARK: - 生命周期

    func start() {
        guard !started else { return }
        started = true
        peripheral.onWrite = { [weak self] bytes in self?.receiveBytes(bytes, source: .ble) }
        peripheral.onSubscribed = { [weak self] in self?.centralSubscribed() }
        peripheral.onUnsubscribed = { [weak self] in self?.centralUnsubscribed() }
        peripheral.onStateChange = { [weak self] state in self?.bluetoothStateChanged(state) }
        peripheral.onEvent = { [weak self] text in self?.log(.system, text) }
        peripheral.start()

        loopbackServer.onReceive = { [weak self] id, bytes in self?.receiveBytes(bytes, source: .loopback(id)) }
        loopbackServer.onConnectionsChanged = { [weak self] count in
            guard let self else { return }
            let increased = count > loopbackClientCount
            loopbackClientCount = count
            if count == 0 {
                loopbackDisconnected()
            } else if increased {
                // loopback 无订阅概念：连接建立即主动推 INFO（§6.1，session=0x00）
                send(engine.makeInfoPush())
            }
        }
        loopbackServer.onEvent = { [weak self] text in self?.log(.system, text) }
        if loopbackWanted {
            loopbackServer.start(port: BuzzLinkConstants.loopbackPort)
        }
        log(.system, "文件库就绪：\(engine.files.count) 个合成样本")
    }

    /// Loopback 开关（UI Toggle）
    func setLoopbackWanted(_ wanted: Bool) {
        loopbackWanted = wanted
        if wanted {
            loopbackServer.start(port: BuzzLinkConstants.loopbackPort)
        } else {
            loopbackServer.stop()
        }
        log(.system, wanted ? "loopback 已开启" : "loopback 已关闭")
    }

    /// 广播开关（UI Toggle）
    func setAdvertisingWanted(_ wanted: Bool) {
        advertisingWanted = wanted
        applyAdvertising()
        log(.system, wanted ? "开始广播 \(engine.profile.deviceName)" : "停止广播")
    }

    func clearLog() {
        logEntries.removeAll()
    }

    // MARK: - BLE 事件

    private func bluetoothStateChanged(_ state: CBManagerState) {
        bluetoothStateText = switch state {
        case .poweredOn: "已开启"
        case .poweredOff: "已关闭"
        case .unauthorized: "未授权"
        case .unsupported: "不支持"
        case .resetting: "重置中"
        default: "未知"
        }
        if state == .poweredOn {
            applyAdvertising()
        }
    }

    private func applyAdvertising() {
        peripheral.setAdvertising(advertisingWanted, deviceName: engine.profile.deviceName)
    }

    private func centralSubscribed() {
        subscribedCentralCount = peripheral.subscribedCentralCount
        log(.system, "Central 已订阅")
        // §6.1：订阅后主动推送 INFO（session=0x00）
        send(engine.makeInfoPush())
    }

    private func centralUnsubscribed() {
        subscribedCentralCount = peripheral.subscribedCentralCount
        log(.system, "Central 已退订")
        ingressContexts.removeValue(forKey: .ble)
        resetSession()
        peripheral.flushPending()
    }

    /// Loopback 客户端全部断开：清理全部 loopback 入站上下文并重置会话
    private func loopbackDisconnected() {
        log(.system, "loopback 客户端已断开")
        for source in ingressContexts.keys {
            if case .loopback = source {
                ingressContexts.removeValue(forKey: source)
            }
        }
        resetSession()
    }

    /// 链路断开后的会话重置（§7：连接中断，全部未完成命令作废）。
    /// Sim 按单链路使用设计；多链路并发时任一链路断开都会重置共享会话。
    private func resetSession() {
        stopStreamLoop()
        engine.reset()
        streamProgress = nil
    }

    // MARK: - 接收链路：字节流 → 帧 → 重组消息 → 引擎

    private func receiveBytes(_ bytes: Data, source: IngressSource) {
        var context = ingressContexts[source]
            ?? IngressContext(parser: BuzzLinkFrameParser(), reassembler: BuzzLinkMessageReassembler())
        context.parser.append(bytes)
        while let frame = context.parser.nextFrame() {
            log(.inbound, Self.describe(frame))
            do {
                if let message = try context.reassembler.accept(frame) {
                    send(engine.handle(message, now: Self.now()))
                }
            } catch {
                log(.system, "分片重组失败：\(error.localizedDescription)")
            }
        }
        ingressContexts[source] = context
        syncStreamLoop()
    }

    // MARK: - 发送链路：引擎输出 →（故障施加）→ 编码 → GATT notify

    private func send(_ outputs: [SimOutboundFrame]) {
        for output in outputs {
            do {
                let bytes = try Self.wireBytes(output)
                // 设备语义：响应同时发往 BLE 订阅者与 loopback 客户端（单链路使用时无差别）
                peripheral.notify(bytes)
                loopbackServer.broadcast(bytes)
                log(.outbound, Self.describe(output.frame, corrupt: output.corruptCRC))
            } catch {
                logger.error("帧编码失败：\(error.localizedDescription)")
                log(.system, "帧编码失败：\(error.localizedDescription)")
            }
        }
        syncStreamLoop()
    }

    /// 编码为线上字节；corruptCRC 故障在此施加（翻转 CRC 字段，TESTING.md §5）
    private static func wireBytes(_ output: SimOutboundFrame) throws -> Data {
        var bytes = try output.frame.encoded()
        if output.corruptCRC {
            let lastIndex = bytes.index(before: bytes.endIndex)
            bytes[lastIndex] ^= 0xFF
        }
        return bytes
    }

    // MARK: - 推流循环（拉取模型：节奏/限速/静默在此执行）

    private func syncStreamLoop() {
        if engine.activeStream != nil, streamTask == nil {
            streamTask = Task { [weak self] in
                await self?.runStreamLoop()
            }
        } else if engine.activeStream == nil, !engine.isSilenced(now: Self.now()) {
            stopStreamLoop()
        }
    }

    private func stopStreamLoop() {
        streamTask?.cancel()
        streamTask = nil
    }

    private func runStreamLoop() async {
        var wasSilenced = false
        while !Task.isCancelled {
            let now = Self.now()
            let silenced = engine.isSilenced(now: now)
            if silenced != wasSilenced {
                wasSilenced = silenced
                isSilencedNow = silenced
                if silenced {
                    // 模拟链路冻结：停止广播，一切 notify 暂停
                    peripheral.setAdvertising(false, deviceName: engine.profile.deviceName)
                    log(.system, "进入静默 \(engine.faults.silentDuration)s（模拟断连）")
                } else {
                    applyAdvertising()
                    log(.system, "静默结束，恢复广播")
                }
            }

            if let output = engine.nextChunk(now: now) {
                updateStreamProgress()
                sendWithoutLoopSync(output)
                if let rate = engine.faults.throttleBytesPerSecond, rate > 0 {
                    let delay = Double(output.frame.payload.count) / Double(rate)
                    try? await Task.sleep(for: .seconds(delay))
                }
            } else if engine.activeStream != nil || engine.isSilenced(now: Self.now()) {
                // 静默中：低频轮询等待恢复
                try? await Task.sleep(for: .milliseconds(silenced ? 500 : 20))
            } else {
                break // 流结束
            }
        }
        streamTask = nil
        streamProgress = nil
        isSilencedNow = false
    }

    private func sendWithoutLoopSync(_ output: SimOutboundFrame) {
        do {
            let bytes = try Self.wireBytes(output)
            peripheral.notify(bytes)
            loopbackServer.broadcast(bytes)
        } catch {
            logger.error("帧编码失败：\(error.localizedDescription)")
        }
    }

    private func updateStreamProgress() {
        if let stream = engine.activeStream {
            streamProgress = "\(stream.fileID) \(stream.nextSeq)/\(stream.totalChunks)"
        }
    }

    // MARK: - 日志

    private func log(_ direction: SimLogEntry.Direction, _ text: String) {
        logEntries.append(SimLogEntry(direction: direction, text: text))
        if logEntries.count > Self.logLimit {
            logEntries.removeFirst(logEntries.count - Self.logLimit)
        }
    }

    private static func describe(_ frame: BuzzLinkFrame, corrupt: Bool = false) -> String {
        let name = switch BuzzLinkFrameType(rawValue: frame.type) {
        case .ping: "PING"
        case .pong: "PONG"
        case .info: "INFO"
        case .infoGet: "INFO_GET"
        case .listReq: "LIST_REQ"
        case .listResp: "LIST_RESP"
        case .fileInfoReq: "FILE_INFO_REQ"
        case .fileInfoResp: "FILE_INFO_RESP"
        case .dlBeginReq: "DL_BEGIN_REQ"
        case .dlBeginResp: "DL_BEGIN_RESP"
        case .dlChunk: "DL_CHUNK"
        case .dlEndReq: "DL_END_REQ"
        case .dlEndResp: "DL_END_RESP"
        case .dlAbort: "DL_ABORT"
        case .fileDeleteReq: "FILE_DELETE_REQ"
        case .fileDeleteResp: "FILE_DELETE_RESP"
        case .error: "ERROR"
        case nil: String(format: "0x%02X", frame.type)
        }
        let session = String(format: "%02X", frame.session)
        let suffix = corrupt ? " [CRC 已损坏]" : ""
        return "\(name) sess=0x\(session) len=\(frame.payload.count)\(suffix)"
    }

    // MARK: - 样本加载

    /// 内置合成样本（tools/make_sim_samples.sh 生成；时长以 ffprobe 实测毫秒为准）。
    /// 打包异常时降级为确定性伪数据文件，保证 Sim 始终可用。
    private static func loadBundledFiles() -> [SimFile] {
        let samples: [(resource: String, id: String, durationMs: UInt32)] = [
            ("sample-05s", "REC_20260101_120000", 5_006),
            ("sample-30s", "REC_20260102_093000", 30_006),
            ("sample-120s", "REC_20260103_074500", 120_006)
        ]
        var files: [SimFile] = []
        for sample in samples {
            let url = Bundle.main.url(forResource: sample.resource, withExtension: "ogg", subdirectory: "Resources/Samples")
                ?? Bundle.main.url(forResource: sample.resource, withExtension: "ogg", subdirectory: "Samples")
                ?? Bundle.main.url(forResource: sample.resource, withExtension: "ogg")
            if let url, let data = try? Data(contentsOf: url) {
                files.append(SimFile(id: sample.id, durationMs: sample.durationMs, data: data))
            }
        }
        if files.isEmpty {
            var rng = SplitMix64(seed: 0x5EED_FA11_BAC0)
            files = samples.map {
                SimFile(
                    id: $0.id,
                    durationMs: $0.durationMs,
                    data: Data((0..<4_096).map { _ in UInt8(truncatingIfNeeded: rng.next()) })
                )
            }
        }
        return files
    }
}
