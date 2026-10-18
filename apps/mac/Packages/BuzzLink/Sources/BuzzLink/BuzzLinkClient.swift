import BuzzLinkCore
import Foundation
import os

/// BuzzLink Central 会话层：状态机、session 分配与匹配、下载重组与 CRC 验收
/// （BUZZLINK.md §3.4/§6/§7；studio-tech-design §4.2）。
///
/// 状态机：`idle → scanning → connecting → ready ⇄ downloading/verifying → ready`，
/// 断连收敛到 `disconnected`（自动重连策略由上层 SyncEngine 决定）。
///
/// 线程模型：actor 隔离。入站字节流经「帧重组（§3.3）→ 分片重组（§3.3）→ 分发」：
/// 命令响应按 session 匹配等待方，DL_CHUNK 喂给进行中的下载，INFO 推送（session=0x00）
/// 单独上报。后台接收/状态循环随传输生命周期运行，任务以弱引用捕获本 actor。
///
/// 可靠性（§7）：文件 CRC 不等、大小不符或块间隔超时 → `DL_ABORT` 清理设备侧后
/// 整文件重下，最多 `maxDownloadAttempts` 趟（默认 = §7 上限 2 次重试 + 首趟）。
public actor BuzzLinkClient {

    /// 状态变化流（去重后产出；初始状态不回放）
    public nonisolated let stateUpdates: AsyncStream<BuzzLinkClientState>
    /// 设备主动推送的 INFO（§6.1，session=0x00）流
    public nonisolated let deviceInfoUpdates: AsyncStream<BuzzLinkDeviceInfo>

    // MARK: - 配置与可观测状态

    /// 命令响应超时（§3.4：5 s）
    public let commandTimeout: TimeInterval
    /// 下载中相邻 DL_CHUNK 间隔超时（§3.4：10 s）
    public let chunkTimeout: TimeInterval
    /// 整文件下载最大尝试趟数（含首趟）
    public let maxDownloadAttempts: Int

    /// 最近收到的设备信息（INFO 推送或 INFO_GET 响应）
    public private(set) var lastDeviceInfo: BuzzLinkDeviceInfo?

    /// 无法匹配到等待方的入站消息帧数（迟到/重复/未知 session；§3.4 容忍）
    public private(set) var unmatchedFrameCount = 0

    /// session 计数器：递增分配、跳过 0x00（INFO 推送专用）、255 后回绕（§3.4）。
    /// internal 仅供测试驱动回绕路径。
    var sessionCounter: UInt8 = 0

    // MARK: - 内部状态

    private let transport: any BuzzLinkTransport
    private let logger = Logger(subsystem: "com.beeuc.beecord.buzzlink", category: "client")

    private var stateValue: BuzzLinkClientState = .idle
    private var lastEmittedState: BuzzLinkClientState?
    private let stateContinuation: AsyncStream<BuzzLinkClientState>.Continuation
    private let deviceInfoContinuation: AsyncStream<BuzzLinkDeviceInfo>.Continuation

    private var parser = BuzzLinkFrameParser()
    private var reassembler = BuzzLinkMessageReassembler()
    private var pendingCommands: [UInt8: ResponseWaiter] = [:]
    private var activeDownload: DownloadContext?
    /// 进行中的 BEGIN 会话：块可能早于（甚至同批于）BEGIN_RESP 处理到达——
    /// 真实链路同一 TCP 突发可复现——此类块暂存，上下文安装后回放
    private var pendingBeginSession: UInt8?
    private var earlyChunks: [UInt8: [BuzzLinkFrame]] = [:]
    /// connect() 等待 `.ready` 的等待器（观察循环驱动状态，保证流内顺序）
    private var readyWaiter: ReadyWaiter?
    /// disconnect() 已同步收敛；传输随后异步送达的 `.disconnected` 为陈旧事件，忽略一次
    /// （否则可能毒化下一次 connect 的 ready 等待或重复 teardown）
    private var isSuppressingDisconnectEvent = false

    // MARK: - 初始化

    public init(
        transport: any BuzzLinkTransport,
        commandTimeout: TimeInterval = BuzzLinkConstants.commandTimeout,
        chunkTimeout: TimeInterval = BuzzLinkConstants.chunkIntervalTimeout,
        maxDownloadAttempts: Int = BuzzLinkConstants.maxFileDownloadRetries + 1
    ) {
        self.transport = transport
        self.commandTimeout = commandTimeout
        self.chunkTimeout = chunkTimeout
        self.maxDownloadAttempts = max(1, maxDownloadAttempts)
        let stateStream = AsyncStream<BuzzLinkClientState>.makeStream()
        self.stateUpdates = stateStream.stream
        self.stateContinuation = stateStream.continuation
        let infoStream = AsyncStream<BuzzLinkDeviceInfo>.makeStream()
        self.deviceInfoUpdates = infoStream.stream
        self.deviceInfoContinuation = infoStream.continuation

        // 后台循环：入站接收 + 传输状态观察。弱引用捕获本 actor；
        // 流（transport 持有 continuation）终止时循环退出，不随 connect/disconnect 启停。
        let inbound = transport.inbound
        let transportStates = transport.stateStream
        Task { [weak self] in
            for await bytes in inbound {
                guard let self else { return }
                await self.ingestInbound(bytes)
            }
        }
        Task { [weak self] in
            for await state in transportStates {
                guard let self else { return }
                await self.observeTransportState(state)
            }
        }
    }

    // MARK: - 连接生命周期

    /// 当前客户端状态
    public var state: BuzzLinkClientState { stateValue }

    /// 建立连接（扫描 → 连接 → 就绪）。失败收敛到 `disconnected` 并抛出。
    ///
    /// 状态流由传输状态观察循环唯一驱动（保证流内顺序）；本方法等待观察循环
    /// 收敛到 `.ready` 后才返回（带 5 s 兜底超时，防传输实现漏发状态）。
    public func connect() async throws {
        switch stateValue {
        case .idle, .disconnected:
            break
        default:
            throw BuzzLinkError.invalidState
        }
        parser = BuzzLinkFrameParser()
        reassembler = BuzzLinkMessageReassembler()
        do {
            try await transport.connect()
        } catch {
            // 丢弃本次失败尝试后续异步送达的陈旧状态事件（.connecting/.ready/.disconnected）
            isSuppressingDisconnectEvent = true
            handleTransportDisconnected()
            throw mapTransportError(error)
        }
        do {
            try await withReadyWaiter()
        } catch {
            handleTransportDisconnected()
            throw error
        }
    }

    /// 断开连接（未完成命令与下载立即以 `.disconnected` 失败；返回时状态已收敛）。
    public func disconnect() {
        isSuppressingDisconnectEvent = true
        transport.disconnect()
        // 同步收敛：传输的 .disconnected 异步到达时由 observeTransportState 忽略一次
        handleTransportDisconnected()
    }

    // MARK: - 链路命令

    /// 保活 PING（§5.1）；等待对应 PONG。
    public func ping() async throws {
        let response = try await runCommand(timeout: commandTimeout) { session in
            BuzzLinkFrame(
                type: BuzzLinkFrameType.ping.rawValue,
                session: session,
                channel: BuzzLinkChannel.link.rawValue
            )
        }
        try throwIfErrorResponse(response)
        guard response.type == BuzzLinkFrameType.pong.rawValue else {
            throw BuzzLinkError.protocolViolation("期望 PONG，实际 type=0x\(String(response.type, radix: 16))")
        }
    }

    // MARK: - 控制通道命令

    /// 请求设备信息（INFO_GET，§5.1）。
    public func getDeviceInfo() async throws -> BuzzLinkDeviceInfo {
        let response = try await runCommand(timeout: commandTimeout) { session in
            BuzzLinkFrame(
                type: BuzzLinkFrameType.infoGet.rawValue,
                session: session,
                channel: BuzzLinkChannel.control.rawValue
            )
        }
        try throwIfErrorResponse(response)
        guard response.type == BuzzLinkFrameType.info.rawValue else {
            throw BuzzLinkError.protocolViolation("期望 INFO，实际 type=0x\(String(response.type, radix: 16))")
        }
        let payload = try decodeJSON(InfoPayload.self, from: response.payload, context: "INFO")
        let info = BuzzLinkDeviceInfo(
            model: payload.model,
            fwVersion: payload.fwVersion,
            protocolVersion: payload.protocolVersion,
            deviceName: payload.deviceName,
            battery: payload.battery,
            isCharging: payload.charging,
            storageTotal: payload.storageTotal,
            storageFree: payload.storageFree
        )
        lastDeviceInfo = info
        return info
    }

    /// 拉取文件列表页（LIST_REQ，§5.1/§8.2；每页最多 16 条，超出按 offset 翻页）。
    /// - Parameters:
    ///   - offset: 起始下标（≥ 0）
    ///   - limit: 每页条数上限（nil = 设备默认 16；>16 由设备截断）
    public func listFiles(offset: Int = 0, limit: Int? = nil) async throws -> BuzzLinkFileListPage {
        guard offset >= 0, limit != 0 else {
            throw BuzzLinkError.invalidParameters
        }
        let response = try await runCommand(timeout: commandTimeout) { session in
            let body = ListRequestPayload(offset: offset, limit: limit)
            let data = try JSONEncoder().encode(body)
            return BuzzLinkFrame(
                type: BuzzLinkFrameType.listReq.rawValue,
                session: session,
                channel: BuzzLinkChannel.control.rawValue,
                payload: data
            )
        }
        try throwIfErrorResponse(response)
        guard response.type == BuzzLinkFrameType.listResp.rawValue else {
            throw BuzzLinkError.protocolViolation("期望 LIST_RESP，实际 type=0x\(String(response.type, radix: 16))")
        }
        let payload = try decodeJSON(ListResponsePayload.self, from: response.payload, context: "LIST_RESP")
        guard payload.status != "error" else {
            throw BuzzLinkError.mapErrorCode(payload.code ?? 4_999, message: payload.message ?? "unknown")
        }
        guard let total = payload.total, let pageOffset = payload.offset, let files = payload.files else {
            throw BuzzLinkError.protocolViolation("LIST_RESP 缺少 total/offset/files 字段")
        }
        return BuzzLinkFileListPage(
            total: total,
            offset: pageOffset,
            files: files.map {
                BuzzLinkFileEntry(id: $0.id, durationMs: $0.durationMs, size: $0.size)
            }
        )
    }

    /// 查询文件元数据（FILE_INFO_REQ，§5.1/§8.3）。
    public func fileInfo(id: String) async throws -> BuzzLinkFileInfo {
        guard !id.isEmpty, id.utf8.count <= BuzzLinkConstants.maxFileIDLength else {
            throw BuzzLinkError.invalidParameters
        }
        let response = try await runCommand(timeout: commandTimeout) { session in
            let body = FileInfoRequestPayload(id: id)
            let data = try JSONEncoder().encode(body)
            return BuzzLinkFrame(
                type: BuzzLinkFrameType.fileInfoReq.rawValue,
                session: session,
                channel: BuzzLinkChannel.control.rawValue,
                payload: data
            )
        }
        try throwIfErrorResponse(response)
        guard response.type == BuzzLinkFrameType.fileInfoResp.rawValue else {
            throw BuzzLinkError.protocolViolation("期望 FILE_INFO_RESP，实际 type=0x\(String(response.type, radix: 16))")
        }
        let payload = try decodeJSON(FileInfoResponsePayload.self, from: response.payload, context: "FILE_INFO_RESP")
        guard payload.status != "error" else {
            throw BuzzLinkError.mapErrorCode(payload.code ?? 4_999, message: payload.message ?? "unknown")
        }
        guard let size = payload.size, let crc16 = payload.crc16, let durationMs = payload.durationMs else {
            throw BuzzLinkError.protocolViolation("FILE_INFO_RESP 缺少 size/crc16/durationMs 字段")
        }
        return BuzzLinkFileInfo(
            id: payload.id ?? id,
            size: size,
            crc16: crc16,
            durationMs: durationMs,
            format: payload.format ?? "",
            sampleRate: payload.sampleRate ?? 0,
            channels: payload.channels ?? 0
        )
    }

    // MARK: - 文件下载

    /// 整文件下载（§6.2）：DL_BEGIN → 收齐 DL_CHUNK → 本地 CRC → DL_END 三处 CRC 裁决。
    ///
    /// 可靠性（§7）：CRC 不等/大小不符/块超时 → DL_ABORT 清理设备侧后整文件重下，
    /// 最多 `maxDownloadAttempts` 趟；趟数耗尽抛 ``BuzzLinkError/downloadRetryExhausted(fileID:attempts:lastError:)``。
    /// 下载进行中控制通道命令仍可并行（§3.4）；并发第二个下载会因状态守卫或设备 busy 失败。
    /// 取消语义（合作式即时取消）：收块循环每块、每趟出发前、BEGIN 成立开工前检查任务取消；
    /// 命中即中断本趟，尽力发 DL_ABORT 清理设备侧推流，状态收敛回 `.ready`，抛 `CancellationError`。
    /// 在飞的单个命令等待不被打断（≤ 命令超时兜底回收）；末段 DL_END 握手期间到达的取消
    /// 不再拦截（文件已验收完成，按成功返回）。
    ///
    /// - Parameters:
    ///   - fileID: 设备文件 ID（≤ 32 字节，§5.2）
    ///   - progress: 进度回调（单趟内 receivedBytes 单调递增；非隔离，回调内自行跳队列）
    /// - Returns: 验收通过的完整文件
    public func download(
        fileID: String,
        progress: (@Sendable (BuzzLinkDownloadProgress) -> Void)? = nil
    ) async throws -> BuzzLinkDownloadResult {
        guard !fileID.isEmpty, fileID.utf8.count <= BuzzLinkConstants.maxFileIDLength else {
            throw BuzzLinkError.invalidParameters
        }
        try requireCommandAllowed(allowDuringTransfer: false)
        return try await downloadWithRetries(fileID: fileID, progress: progress)
    }

    // MARK: - 测试辅助（internal，不进公开 API）

    /// 直接设置 session 计数器（回绕路径测试用）
    func setSessionCounterForTest(_ value: UInt8) {
        sessionCounter = value
    }

    // MARK: - 私有：下载

    private func downloadWithRetries(
        fileID: String,
        progress: (@Sendable (BuzzLinkDownloadProgress) -> Void)?
    ) async throws -> BuzzLinkDownloadResult {
        var lastError: BuzzLinkError = .internalFailure("未执行任何下载尝试")
        for attempt in 1...maxDownloadAttempts {
            do {
                // 趟间合作式取消点：首趟/重试出发前即响应取消，不再发起新的一趟
                try Task.checkCancellation()
                return try await performDownloadAttempt(
                    fileID: fileID,
                    attempt: attempt,
                    progress: progress
                )
            } catch is CancellationError {
                await sendAbortBestEffort(fileID: fileID)
                throw CancellationError()
            } catch let error as BuzzLinkError {
                lastError = error
                guard error.allowsDownloadRetry else {
                    throw error
                }
                guard attempt < maxDownloadAttempts else {
                    throw BuzzLinkError.downloadRetryExhausted(
                        fileID: fileID,
                        attempts: attempt,
                        lastError: error
                    )
                }
                logger.info("下载 \(fileID) 第 \(attempt) 趟失败（\(error.localizedDescription)），DL_ABORT 后重试")
                await sendAbortBestEffort(fileID: fileID)
            }
        }
        throw lastError
    }

    /// 单趟下载：BEGIN → CHUNK×N → 重组/CRC → END。
    private func performDownloadAttempt(
        fileID: String,
        attempt: Int,
        progress: (@Sendable (BuzzLinkDownloadProgress) -> Void)?
    ) async throws -> BuzzLinkDownloadResult {
        // 1. DL_BEGIN（§6.2；chunkDataLen/totalChunks/size/fileCrc16 以响应声明为准）
        defer {
            // 尝试结束（含 BEGIN 失败路径）：清暂存与 BEGIN 会话标记
            if let session = pendingBeginSession {
                earlyChunks.removeValue(forKey: session)
                pendingBeginSession = nil
            }
        }
        let beginResponse = try await runCommand(timeout: commandTimeout) { session in
            self.pendingBeginSession = session
            return BuzzLinkFrame(
                type: BuzzLinkFrameType.dlBeginReq.rawValue,
                session: session,
                channel: BuzzLinkChannel.file.rawValue,
                payload: Self.fileIDPayload(fileID)
            )
        }
        try throwIfErrorResponse(beginResponse)
        guard beginResponse.type == BuzzLinkFrameType.dlBeginResp.rawValue else {
            throw BuzzLinkError.protocolViolation(
                "期望 DL_BEGIN_RESP，实际 type=0x\(String(beginResponse.type, radix: 16))"
            )
        }
        let begin = try parseBeginResponse(beginResponse.payload)
        let beginSession = beginResponse.session

        // BEGIN 成立、推流即将开始：开工前最后取消点，避免为已放弃的下载建立推流
        try Task.checkCancellation()

        emitState(.downloading(fileID: fileID))

        // 2. 收块：DL_CHUNK 经分发器进入块流；看门狗负责块间隔超时
        let ctx = DownloadContext(
            fileID: fileID,
            session: beginSession,
            chunkDataLen: begin.chunkDataLen,
            totalChunks: begin.totalChunks,
            size: begin.size,
            beginFileCRC: begin.fileCRC16,
            startedAt: Self.now()
        )
        activeDownload = ctx
        if let buffered = earlyChunks.removeValue(forKey: beginSession) {
            for chunk in buffered {
                ctx.chunkContinuation.yield(chunk)
            }
        }
        ctx.watchdog = Task { [weak self] in
            await self?.downloadWatchdog(session: beginSession)
        }
        defer { clearDownload(session: beginSession) }

        var iterator = ctx.chunkStream.makeAsyncIterator()
        while ctx.chunks.count < ctx.totalChunks {
            // 收块循环合作式取消点：取消即中断本趟（DL_ABORT 由上层重试循环统一补发）
            try Task.checkCancellation()
            guard let message = await iterator.next() else {
                // AsyncStream 对消费方任务取消以返回 nil 响应：已记录的失败原因优先抛出；
                // 无失败原因且任务已取消 → 统一上报 CancellationError
                if let failure = ctx.failure { throw failure }
                try Task.checkCancellation()
                throw BuzzLinkError.protocolViolation("块流意外结束")
            }
            recordChunk(message, in: ctx, attempt: attempt, progress: progress)
        }

        // 3. 按 seq 重组为 size 字节，长度与 CRC 裁决（§6.2/§6.3）
        guard ctx.receivedBytes == ctx.size else {
            throw BuzzLinkError.sizeMismatch(expected: ctx.size, actual: ctx.receivedBytes)
        }
        var assembled = Data()
        assembled.reserveCapacity(ctx.size)
        for seq in 0..<ctx.totalChunks {
            guard let chunk = ctx.chunks[seq] else {
                throw BuzzLinkError.protocolViolation("缺块 seq=\(seq)")
            }
            assembled.append(chunk)
        }
        let localCRC = CRC16.ccittFalse(assembled)
        guard localCRC == ctx.beginFileCRC else {
            throw BuzzLinkError.crcMismatch(expected: ctx.beginFileCRC, actual: localCRC)
        }

        // 4. DL_END：三处 CRC 全等方验收通过（§6.2）
        emitState(.verifying(fileID: fileID))
        let endResponse = try await runCommand(timeout: commandTimeout) { session in
            BuzzLinkFrame(
                type: BuzzLinkFrameType.dlEndReq.rawValue,
                session: session,
                channel: BuzzLinkChannel.file.rawValue,
                payload: Self.fileIDPayload(fileID)
            )
        }
        try throwIfErrorResponse(endResponse)
        guard endResponse.type == BuzzLinkFrameType.dlEndResp.rawValue,
              endResponse.payload.count == 3 else {
            throw BuzzLinkError.protocolViolation("DL_END_RESP 结构非法")
        }
        let endStatusByte = endResponse.payload[endResponse.payload.startIndex]
        guard let endStatus = BuzzLinkStatus(rawValue: endStatusByte), endStatus == .ok else {
            throw BuzzLinkError.mapStatus(BuzzLinkStatus(rawValue: endStatusByte) ?? .internalError)
        }
        let endCRC = endResponse.payload.readClientLE16(at: endResponse.payload.startIndex + 1)
        guard endCRC == localCRC else {
            throw BuzzLinkError.crcMismatch(expected: endCRC, actual: localCRC)
        }
        return BuzzLinkDownloadResult(fileID: fileID, data: assembled, size: assembled.count, crc16: localCRC)
    }

    /// 记录一块 DL_CHUNK：容忍跳号/重复/乱序（§7），重复块忽略，最终以 CRC 裁决。
    private func recordChunk(
        _ message: BuzzLinkFrame,
        in ctx: DownloadContext,
        attempt: Int,
        progress: (@Sendable (BuzzLinkDownloadProgress) -> Void)?
    ) {
        ctx.lastChunkAt = Self.now()
        guard message.type == BuzzLinkFrameType.dlChunk.rawValue, message.session == ctx.session else { return }
        guard message.payload.count >= 2 else { return }
        let seq = Int(message.payload.readClientLE16(at: message.payload.startIndex))
        let data = message.payload.dropFirst(2)
        guard seq < ctx.totalChunks, data.count <= ctx.chunkDataLen else { return }
        guard ctx.chunks[seq] == nil else { return }
        ctx.chunks[seq] = Data(data)
        ctx.receivedBytes += data.count
        progress?(
            BuzzLinkDownloadProgress(
                fileID: ctx.fileID,
                attempt: attempt,
                receivedBytes: ctx.receivedBytes,
                totalBytes: ctx.size
            )
        )
    }

    /// 块间隔看门狗（§3.4：10 s 无块即判失败）。
    private func downloadWatchdog(session: UInt8) async {
        while !Task.isCancelled {
            try? await Task.sleep(for: .milliseconds(Self.milliseconds(of: chunkTimeout)))
            guard !Task.isCancelled else { return }
            guard let ctx = activeDownload, ctx.session == session else { return }
            if Self.now() - ctx.lastChunkAt >= chunkTimeout {
                ctx.failure = .chunkTimeout(fileID: ctx.fileID)
                ctx.chunkContinuation.finish()
                return
            }
        }
    }

    private func clearDownload(session: UInt8) {
        guard let ctx = activeDownload, ctx.session == session else { return }
        ctx.watchdog?.cancel()
        ctx.chunkContinuation.finish()
        activeDownload = nil
        switch stateValue {
        case .downloading(let id) where id == ctx.fileID,
             .verifying(let id) where id == ctx.fileID:
            emitState(.ready)
        default:
            break
        }
    }

    /// 尽力清理设备侧推流状态（§7）；不等待响应（设备可能正处于故障静默）。
    private func sendAbortBestEffort(fileID: String) async {
        do {
            let frame = BuzzLinkFrame(
                type: BuzzLinkFrameType.dlAbort.rawValue,
                session: nextSession(),
                channel: BuzzLinkChannel.file.rawValue,
                payload: Self.fileIDPayload(fileID)
            )
            try await transport.send(frame.encoded())
        } catch {
            logger.debug("DL_ABORT 发送失败（忽略，继续重试）：\(error.localizedDescription)")
        }
    }

    // MARK: - 私有：命令收发

    /// 发出一条命令并等待同 session 响应；超时按 `timeout` 计（§3.4）。
    /// 发送任意构造的帧（T7 协议控制台通道：预置命令与自定义 hex 共用）。
    ///
    /// 语义与高层命令一致（``requireCommandAllowed(allowDuringTransfer:)`` 默认放行传输期）：
    /// session 由客户端分配（覆写 `frame.session`，与 ``nextSession`` 单调序列同源）；
    /// `awaitResponse=true` 时按 session 匹配响应（``ResponseWaiter`` + 超时沿用
    /// `commandTimeout`），超时/断连错误同 ``runCommand``；`false` 为发送即返回 nil，
    /// 响应经旁听流（`FrameTapTransport/events`）观测。发送期断连即时收敛处理同 ``runCommand``。
    public func sendRaw(_ frame: BuzzLinkFrame, awaitResponse: Bool = true) async throws -> BuzzLinkFrame? {
        try requireCommandAllowed()
        let session = nextSession()
        let request = BuzzLinkFrame(
            type: frame.type, session: session, flags: frame.flags,
            channel: frame.channel, payload: frame.payload
        )
        guard awaitResponse else {
            // fire-and-forget：不注册 waiter（响应由控制台旁听流呈现）
            do {
                try await transport.send(request.encoded())
            } catch {
                let mapped = mapTransportError(error)
                if mapped == .disconnected {
                    handleTransportDisconnected()
                }
                throw mapped
            }
            return nil
        }
        let waiter = ResponseWaiter()
        pendingCommands[session] = waiter
        do {
            try await transport.send(request.encoded())
        } catch {
            pendingCommands.removeValue(forKey: session)
            let mapped = mapTransportError(error)
            waiter.settle(with: .failure(mapped))
            if mapped == .disconnected {
                // 发送期断连：立即收敛（传输的 .disconnected 状态稍后到达时幂等去重）
                handleTransportDisconnected()
            }
            throw mapped
        }
        let timeout = commandTimeout
        let timeoutTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(Self.milliseconds(of: timeout)))
            guard !Task.isCancelled else { return }
            await self?.timeoutCommand(session: session)
        }
        do {
            let response = try await waiter.value()
            timeoutTask.cancel()
            return response
        } catch {
            timeoutTask.cancel()
            throw error
        }
    }

    private func runCommand(
        timeout: TimeInterval,
        build: (UInt8) throws -> BuzzLinkFrame
    ) async throws -> BuzzLinkFrame {
        try requireCommandAllowed()
        let session = nextSession()
        let request: BuzzLinkFrame
        do {
            request = try build(session)
        } catch {
            throw BuzzLinkError.invalidParameters
        }
        let waiter = ResponseWaiter()
        pendingCommands[session] = waiter
        do {
            try await transport.send(request.encoded())
        } catch {
            pendingCommands.removeValue(forKey: session)
            let mapped = mapTransportError(error)
            waiter.settle(with: .failure(mapped))
            if mapped == .disconnected {
                // 发送期断连：立即收敛（传输的 .disconnected 状态稍后到达时幂等去重）
                handleTransportDisconnected()
            }
            throw mapped
        }
        let timeoutTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(Self.milliseconds(of: timeout)))
            guard !Task.isCancelled else { return }
            await self?.timeoutCommand(session: session)
        }
        do {
            let response = try await waiter.value()
            timeoutTask.cancel()
            return response
        } catch {
            timeoutTask.cancel()
            throw error
        }
    }

    /// 超时：移除等待方并以 commandTimeout 完成（恰好一次）。
    private func timeoutCommand(session: UInt8) {
        guard let waiter = pendingCommands.removeValue(forKey: session) else { return }
        waiter.settle(with: .failure(BuzzLinkError.commandTimeout(session: session)))
    }

    private func requireCommandAllowed(allowDuringTransfer: Bool = true) throws {
        switch stateValue {
        case .ready:
            return
        case .downloading, .verifying:
            if allowDuringTransfer { return }
            throw BuzzLinkError.invalidState
        case .idle, .disconnected:
            throw BuzzLinkError.notConnected
        case .scanning, .connecting:
            throw BuzzLinkError.invalidState
        }
    }

    private func nextSession() -> UInt8 {
        sessionCounter &+= 1
        if sessionCounter == 0 { sessionCounter = 1 }
        return sessionCounter
    }

    /// ERROR 帧（0x7F）统一映射（§9）。
    private func throwIfErrorResponse(_ frame: BuzzLinkFrame) throws {
        guard frame.type == BuzzLinkFrameType.error.rawValue else { return }
        let payload = try decodeJSON(ErrorPayload.self, from: frame.payload, context: "ERROR")
        throw BuzzLinkError.mapErrorCode(payload.code, message: payload.message)
    }

    /// DL_BEGIN_RESP 13 字节定长 payload（§5.1）。
    private func parseBeginResponse(_ payload: Data) throws -> (
        chunkDataLen: Int, totalChunks: Int, size: Int, fileCRC16: UInt16
    ) {
        guard payload.count == 13 else {
            throw BuzzLinkError.protocolViolation("DL_BEGIN_RESP 应为 13 字节，实际 \(payload.count)")
        }
        let statusByte = payload[payload.startIndex]
        guard let status = BuzzLinkStatus(rawValue: statusByte), status == .ok else {
            throw BuzzLinkError.mapStatus(BuzzLinkStatus(rawValue: statusByte) ?? .internalError)
        }
        let chunkDataLen = Int(payload.readClientLE16(at: payload.startIndex + 1))
        let totalChunks = Int(payload.readClientLE32(at: payload.startIndex + 3))
        let size = Int(payload.readClientLE32(at: payload.startIndex + 7))
        let fileCRC16 = payload.readClientLE16(at: payload.startIndex + 11)
        guard chunkDataLen >= 1, chunkDataLen <= BuzzLinkConstants.maxChunkDataLength else {
            throw BuzzLinkError.protocolViolation("DL_BEGIN_RESP 声明的 chunkDataLen=\(chunkDataLen) 非法")
        }
        guard totalChunks == (size + chunkDataLen - 1) / chunkDataLen else {
            throw BuzzLinkError.protocolViolation(
                "DL_BEGIN_RESP totalChunks=\(totalChunks) 与 size=\(size)/chunkDataLen=\(chunkDataLen) 不一致"
            )
        }
        return (chunkDataLen, totalChunks, size, fileCRC16)
    }

    // MARK: - 私有：入站分发

    private func ingestInbound(_ bytes: Data) {
        parser.append(bytes)
        while let frame = parser.nextFrame() {
            do {
                if let message = try reassembler.accept(frame) {
                    dispatch(message)
                }
            } catch {
                // 分片重组错误：reassembler 已丢弃该消息的部分状态，继续后续帧
                logger.error("分片重组失败：\(error.localizedDescription)")
            }
        }
    }

    /// 完整消息帧 → 下载块流 / INFO 推送 / 命令响应匹配。
    private func dispatch(_ message: BuzzLinkFrame) {
        if message.type == BuzzLinkFrameType.dlChunk.rawValue {
            if let ctx = activeDownload, message.session == ctx.session {
                ctx.lastChunkAt = Self.now()
                ctx.chunkContinuation.yield(message)
            } else if message.session == pendingBeginSession {
                // BEGIN 已发出、上下文未安装（同链路突发）：暂存待回放
                earlyChunks[message.session, default: []].append(message)
            }
            // 其余块（无进行中下载/非本会话）为陈旧流，静默丢弃（§7 以 CRC 终裁）
            return
        }
        if message.type == BuzzLinkFrameType.info.rawValue, message.session == 0 {
            if let payload = try? decodeJSON(InfoPayload.self, from: message.payload, context: "INFO"),
               let info = BuzzLinkDeviceInfo(payload: payload) {
                lastDeviceInfo = info
                deviceInfoContinuation.yield(info)
            }
            return
        }
        if let waiter = pendingCommands.removeValue(forKey: message.session) {
            waiter.settle(with: .success(message))
        } else {
            unmatchedFrameCount += 1
        }
    }

    private func handleTransportDisconnected() {
        let waiters = pendingCommands
        pendingCommands.removeAll()
        for (_, waiter) in waiters {
            waiter.settle(with: .failure(BuzzLinkError.disconnected))
        }
        if let ctx = activeDownload {
            ctx.failure = .disconnected
            ctx.watchdog?.cancel()
            ctx.chunkContinuation.finish()
            activeDownload = nil
        }
        readyWaiter?.fail(BuzzLinkError.disconnected)
        readyWaiter = nil
        earlyChunks.removeAll()
        parser.reset()
        reassembler.reset()
        emitState(.disconnected)
    }

    private func observeTransportState(_ state: BuzzLinkTransportState) {
        switch state {
        case .idle:
            break
        case .scanning:
            emitState(.scanning)
        case .connecting:
            emitState(.connecting)
        case .ready:
            emitState(.ready)
        case .disconnected:
            if isSuppressingDisconnectEvent {
                // disconnect() 已同步收敛，本事件为其传输侧回声，忽略
                isSuppressingDisconnectEvent = false
                return
            }
            handleTransportDisconnected()
        }
    }

    // MARK: - 私有：杂项

    private func emitState(_ newState: BuzzLinkClientState) {
        stateValue = newState
        if newState == .ready {
            readyWaiter?.succeed()
        }
        if lastEmittedState != newState {
            lastEmittedState = newState
            stateContinuation.yield(newState)
        }
    }

    /// 等待观察循环把状态收敛到 `.ready`（5 s 兜底，防传输实现漏发状态）。
    private func withReadyWaiter() async throws {
        let waiter = ReadyWaiter()
        readyWaiter = waiter
        if stateValue == .ready {
            waiter.succeed()
        }
        let timeoutTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(5_000))
            guard !Task.isCancelled else { return }
            await self?.timeoutReadyWaiter()
        }
        do {
            try await waiter.wait()
            timeoutTask.cancel()
        } catch {
            timeoutTask.cancel()
            throw error
        }
    }

    private func timeoutReadyWaiter() {
        readyWaiter?.fail(BuzzLinkError.transport("connect 后未在 5 s 内就绪"))
        readyWaiter = nil
    }

    private func mapTransportError(_ error: any Error) -> BuzzLinkError {
        if let buzzError = error as? BuzzLinkError {
            return buzzError
        }
        return .transport(String(describing: error))
    }

    private func decodeJSON<T: Decodable>(
        _ type: T.Type,
        from data: Data,
        context: String
    ) throws -> T {
        do {
            return try JSONDecoder().decode(T.self, from: data)
        } catch {
            throw BuzzLinkError.protocolViolation("\(context) JSON 解析失败：\(error.localizedDescription)")
        }
    }

    /// `[idLen u8][id…]`（§5.1）；长度上限由调用方保证。
    private static func fileIDPayload(_ fileID: String) -> Data {
        var payload = Data()
        payload.append(UInt8(fileID.utf8.count))
        payload.append(Data(fileID.utf8))
        return payload
    }

    private static func now() -> TimeInterval {
        ProcessInfo.processInfo.systemUptime
    }

    private static func milliseconds(of interval: TimeInterval) -> Int {
        max(1, Int(interval * 1000))
    }
}

// MARK: - JSON 数据模型（BUZZLINK.md §8；与 BuzzLinkSimCore 的输出同构）

private struct InfoPayload: Decodable {
    let model: String
    let fwVersion: String
    let protocolVersion: Int
    let deviceName: String
    let battery: Int
    let charging: Bool
    let storageTotal: UInt64
    let storageFree: UInt64

    enum CodingKeys: String, CodingKey {
        case model, fwVersion, deviceName, battery, charging, storageTotal, storageFree
        case protocolVersion = "protocol"
    }
}

private extension BuzzLinkDeviceInfo {
    init?(payload: InfoPayload) {
        self.init(
            model: payload.model,
            fwVersion: payload.fwVersion,
            protocolVersion: payload.protocolVersion,
            deviceName: payload.deviceName,
            battery: payload.battery,
            isCharging: payload.charging,
            storageTotal: payload.storageTotal,
            storageFree: payload.storageFree
        )
    }
}

private struct ListRequestPayload: Encodable {
    let offset: Int
    let limit: Int?

    init(offset: Int, limit: Int?) {
        self.offset = offset
        // limit 为 nil 时完全不输出该键（§5.1 可选字段）
        self.limit = limit
    }
}

private struct FileInfoRequestPayload: Encodable {
    let id: String
}

private struct FileEntryPayload: Decodable {
    let id: String
    let durationMs: UInt32
    let size: Int
}

private struct ListResponsePayload: Decodable {
    let status: String
    let total: Int?
    let offset: Int?
    let files: [FileEntryPayload]?
    let code: Int?
    let message: String?
}

private struct FileInfoResponsePayload: Decodable {
    let status: String
    let id: String?
    let size: Int?
    let crc16: UInt16?
    let durationMs: UInt32?
    let format: String?
    let sampleRate: Int?
    let channels: Int?
    let code: Int?
    let message: String?
}

private struct ErrorPayload: Decodable {
    let code: Int
    let message: String
}

// MARK: - 下载上下文

/// 单趟下载上下文。成员仅 actor 隔离访问（类引用仅为跨 await 持有）。
private final class DownloadContext: @unchecked Sendable {

    let fileID: String
    /// DL_BEGIN 的 session；DL_CHUNK 沿用（§3.4）
    let session: UInt8
    /// BEGIN 声明的块数据长度（客户端必须按声明处理，§6.2）
    let chunkDataLen: Int
    let totalChunks: Int
    let size: Int
    let beginFileCRC: UInt16

    let chunkStream: AsyncStream<BuzzLinkFrame>
    let chunkContinuation: AsyncStream<BuzzLinkFrame>.Continuation

    /// 已收块（seq → 数据）；跳号/乱序容忍，重复忽略（§7）
    var chunks: [Int: Data] = [:]
    var receivedBytes = 0
    var lastChunkAt: TimeInterval
    /// 块流终止原因（看门狗超时/断连/取消）；nil = 正常收齐
    var failure: BuzzLinkError?
    var watchdog: Task<Void, Never>?

    init(
        fileID: String,
        session: UInt8,
        chunkDataLen: Int,
        totalChunks: Int,
        size: Int,
        beginFileCRC: UInt16,
        startedAt: TimeInterval
    ) {
        self.fileID = fileID
        self.session = session
        self.chunkDataLen = chunkDataLen
        self.totalChunks = totalChunks
        self.size = size
        self.beginFileCRC = beginFileCRC
        let stream = AsyncStream<BuzzLinkFrame>.makeStream()
        self.chunkStream = stream.stream
        self.chunkContinuation = stream.continuation
        self.lastChunkAt = startedAt
    }
}

// MARK: - 命令响应等待盒

/// 一次性响应等待盒：分发（actor 内）与超时任务竞争完成，NSLock 保证恰好 resume 一次；
/// `settle` 早于 `value()` 挂起时暂存结果，不会挂死。
private final class ResponseWaiter: @unchecked Sendable {

    private let lock = NSLock()
    private var continuation: CheckedContinuation<BuzzLinkFrame, any Error>?
    private var settledResult: Result<BuzzLinkFrame, any Error>?

    func settle(with result: Result<BuzzLinkFrame, any Error>) {
        lock.lock()
        if let continuation {
            self.continuation = nil
            lock.unlock()
            continuation.resume(with: result)
        } else {
            settledResult = result
            lock.unlock()
        }
    }

    func value() async throws -> BuzzLinkFrame {
        try await withCheckedThrowingContinuation { continuation in
            lock.lock()
            if let settledResult {
                self.settledResult = nil
                lock.unlock()
                continuation.resume(with: settledResult)
            } else {
                self.continuation = continuation
                lock.unlock()
            }
        }
    }
}

// MARK: - connect() ready 等待盒

/// connect() 的一次性 ready 等待盒：emitState(.ready) / 断连 / 5 s 超时竞争完成，
/// NSLock 保证恰好 resume 一次；`succeed`/`fail` 早于 `wait()` 时结果暂存不挂死。
private final class ReadyWaiter: @unchecked Sendable {

    private let lock = NSLock()
    private var continuation: CheckedContinuation<Void, any Error>?
    private var settledResult: Result<Void, any Error>?

    func succeed() {
        settle(with: .success(()))
    }

    func fail(_ error: any Error) {
        settle(with: .failure(error))
    }

    func wait() async throws {
        try await withCheckedThrowingContinuation { continuation in
            lock.lock()
            if let settledResult {
                self.settledResult = nil
                lock.unlock()
                continuation.resume(with: settledResult)
            } else {
                self.continuation = continuation
                lock.unlock()
            }
        }
    }

    private func settle(with result: Result<Void, any Error>) {
        lock.lock()
        if let continuation {
            self.continuation = nil
            lock.unlock()
            continuation.resume(with: result)
        } else {
            settledResult = result
            lock.unlock()
        }
    }
}

// MARK: - 小端读取（Central 自备私有 helper；不依赖 BuzzLinkCore 的 module 内部 API）

private extension Data {
    /// 以小端序读取 16 位无符号整数；调用方保证 `offset + 2 <= count`
    func readClientLE16(at offset: Int) -> UInt16 {
        UInt16(self[offset]) | (UInt16(self[offset + 1]) << 8)
    }

    /// 以小端序读取 32 位无符号整数；调用方保证 `offset + 4 <= count`
    func readClientLE32(at offset: Int) -> UInt32 {
        UInt32(self[offset])
            | (UInt32(self[offset + 1]) << 8)
            | (UInt32(self[offset + 2]) << 16)
            | (UInt32(self[offset + 3]) << 24)
    }
}
