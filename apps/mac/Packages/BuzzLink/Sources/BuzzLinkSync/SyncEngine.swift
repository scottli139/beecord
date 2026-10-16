import BuzzLink
import Foundation
import os

/// 同步引擎（studio-tech-design §5.2）：连接生命周期、自动同步 diff、队列化下载、
/// 进度聚合、取消与自动重连、空闲保活。
///
/// 线程模型：actor 隔离。聚合状态经 ``snapshots`` 流（去重后产出，初始值不回放——
/// 用 ``snapshot`` 读当前值）；离散通知（toast 语义）经 ``events`` 流。client 的单文件
/// 进度回调（`@Sendable` 非隔离）写入线程安全进度盒，由 actor 侧节流泵聚合后发快照，
/// 避免每块一次 actor hop（BLE 24KB/s ≈ 107 块/s）。
///
/// 取消语义：``cancelSync()`` 取消当前下载任务（BuzzLinkClient 合作式即时取消），
/// `.cancelling` 态有界等待（``SyncConfig/cancelConvergenceTimeout``），超时兜底断开并
/// 自动重连。epoch 代数在取消/断连/新会话时递增，迟到结果与状态迁移据此丢弃。
///
/// 自动重连：仅"曾经 ready 的会话"掉线触发（首连失败不自动重试）；按
/// ``SyncConfig/reconnectDelays`` 退避，耗尽回 `.idle` 并发 ``SyncEvent/reconnectExhausted``。
/// 空闲保活：仅 `.ready` 态按 ``SyncConfig/keepaliveInterval`` 发 PING（BUZZLINK §6.1，
/// 下载/同步期流量即保活）；ping 失败按断连处理（断开 → 自动重连）。
public actor SyncEngine {

    /// 状态/行/进度快照流（去重后产出）
    public nonisolated let snapshots: AsyncStream<SyncSnapshot>
    /// 离散事件流
    public nonisolated let events: AsyncStream<SyncEvent>

    private let client: BuzzLinkClient
    private let store: any LibraryStore
    private let config: SyncConfig
    private let logger = Logger(subsystem: "com.beeuc.beecord.buzzlink", category: "sync")

    private var snapshotValue = SyncSnapshot(
        state: .idle, rows: [], progress: nil, deviceInfo: nil, isStorageNearlyFull: false
    )
    private var lastEmittedSnapshot: SyncSnapshot?
    private let snapshotContinuation: AsyncStream<SyncSnapshot>.Continuation
    private let eventContinuation: AsyncStream<SyncEvent>.Continuation

    private var autoSyncEnabled: Bool
    /// 会话代数：取消/断连/新会话递增，旧流程的状态迁移据此放弃
    private var epoch = 0
    /// 曾经 ready 过：自动重连资格（首连失败不自动重试）
    private var hasEverBeenReady = false
    /// 用户主动断开：抑制一次自动重连
    private var suppressAutoReconnect = false

    private var sessionTask: Task<Void, Never>?
    private var reconnectTask: Task<Void, Never>?
    private var keepaliveTask: Task<Void, Never>?
    /// 会话流程存活标记（取消的有界等待用）
    private var sessionRunning = false
    /// 下载队列进度态（仅 syncing 期间非空）
    private var queueState: QueueState?

    /// 当前快照
    public var snapshot: SyncSnapshot { snapshotValue }

    public init(client: BuzzLinkClient, store: any LibraryStore, config: SyncConfig = .default) {
        self.client = client
        self.store = store
        self.config = config
        self.autoSyncEnabled = config.autoSyncEnabled
        let snapshotStream = AsyncStream<SyncSnapshot>.makeStream()
        self.snapshots = snapshotStream.stream
        self.snapshotContinuation = snapshotStream.continuation
        let eventStream = AsyncStream<SyncEvent>.makeStream()
        self.events = eventStream.stream
        self.eventContinuation = eventStream.continuation
        let clientStates = client.stateUpdates
        // 客户端状态观察循环：弱引用捕获本 actor，随 client 流生命周期运行
        // （同 BuzzLinkClient 的既定模式；流终止/引擎释放时循环退出）
        Task { [weak self] in
            for await state in clientStates {
                guard let self else { return }
                await self.handleClientState(state)
            }
        }
    }

    // MARK: - 公开 API

    /// 建立连接并启动连接后流程（设备信息 → 列表 → diff → 自动同步开启时入队下载）。
    /// 幂等：非 `.idle` 状态下调用直接返回。首连失败回 `.idle` 并发
    /// ``SyncEvent/syncFailed``（不自动重试，由用户手动重连）。
    public func connect() async {
        guard snapshotValue.state == .idle else { return }
        // 启动清理：.partial 存在即未完成（P0 无断点续传，重连后整文件重下）
        do {
            try await store.purgeStalePartials()
        } catch {
            logger.error("清理 .partial 残留失败（继续连接）：\(error.localizedDescription)")
        }
        setState(.connecting)
        do {
            try await client.connect()
        } catch {
            // 用户中途主动断开：状态已被 disconnect() 收敛，不再报失败
            guard snapshotValue.state == .connecting else { return }
            setState(.idle)
            emitEvent(.syncFailed(error.localizedDescription))
            return
        }
        hasEverBeenReady = true
        startSessionFlow()
    }

    /// 主动断开：取消在飞同步与自动重连，状态回 `.idle`；下载中行标"未完成"。
    public func disconnect() async {
        suppressAutoReconnect = true
        hasEverBeenReady = false
        epoch &+= 1
        reconnectTask?.cancel()
        sessionTask?.cancel()
        stopKeepalive()
        queueState = nil
        setProgress(nil)
        markDownloadingRowsIncomplete()
        await client.disconnect()
        setState(.idle)
    }

    /// 重新拉列表 + diff；自动同步开启时直接入队。仅 `.ready`/`.failed` 态有效。
    public func refresh() async {
        guard snapshotValue.state == .ready || isFailed else { return }
        epoch &+= 1
        startSessionFlow()
    }

    /// 手动下载所选（自动同步关闭时的补充路径；已 verified 的行自动跳过）。
    public func downloadSelected(_ fileIDs: [String]) async {
        guard snapshotValue.state == .ready || isFailed else { return }
        let wanted = snapshotValue.rows.filter {
            fileIDs.contains($0.id) && $0.status != .verified
        }.map(\.id)
        guard !wanted.isEmpty else { return }
        epoch &+= 1
        startDownloadFlow(ids: wanted)
    }

    /// 行内重试（failed / incomplete 行）。
    public func retry(_ fileID: String) async {
        guard snapshotValue.state == .ready || isFailed else { return }
        guard let row = snapshotValue.rows.first(where: { $0.id == fileID }) else { return }
        switch row.status {
        case .failed, .incomplete:
            break
        default:
            return
        }
        setRowStatus(fileID, .notDownloaded)
        epoch &+= 1
        startDownloadFlow(ids: [fileID])
    }

    /// 取消当前同步：当前文件合作式中断，队列清空；下载中行回"未下载"；状态回 `.ready`。
    /// 有界等待 ``SyncConfig/cancelConvergenceTimeout``；超时兜底断开并自动重连。
    public func cancelSync() async {
        guard snapshotValue.state == .syncing || snapshotValue.state == .listing else { return }
        epoch &+= 1
        let cancelEpoch = epoch
        setState(.cancelling)
        sessionTask?.cancel()
        let deadline = Self.now() + config.cancelConvergenceTimeout
        while sessionRunning, Self.now() < deadline {
            try? await Task.sleep(for: .milliseconds(10))
        }
        // 等待期间用户断开/断连：状态由对应路径接管，本调用不再置态
        guard epoch == cancelEpoch else { return }
        guard sessionRunning == false else {
            // 兜底：强拆链路，走自动重连恢复（非用户断开，不抑制）
            let timeout = config.cancelConvergenceTimeout
            logger.error("取消收敛超时（\(timeout)s），断开重连")
            await client.disconnect()
            return
        }
        queueState = nil
        setProgress(nil)
        for row in snapshotValue.rows where row.status.isDownloading {
            setRowStatus(row.id, .notDownloaded)
        }
        setState(.ready)
    }

    /// 自动同步开关；开启时若空闲且有待下载文件，立即触发一轮 diff + 入队。
    public func setAutoSyncEnabled(_ enabled: Bool) async {
        autoSyncEnabled = enabled
        guard enabled, snapshotValue.state == .ready else { return }
        guard snapshotValue.rows.contains(where: { $0.status == .notDownloaded }) else { return }
        epoch &+= 1
        startSessionFlow()
    }

    // MARK: - 私有：会话流程（连接后/刷新/重连共用）

    private func startSessionFlow() {
        sessionTask?.cancel()
        let flowEpoch = epoch
        sessionTask = Task { [weak self] in
            guard let self else { return }
            await self.runSessionFlow(epoch: flowEpoch)
        }
    }

    private func runSessionFlow(epoch flowEpoch: Int) async {
        sessionRunning = true
        defer { sessionRunning = false }
        do {
            setState(.listing)
            let info = try await client.getDeviceInfo()
            try epochGuard(flowEpoch)
            setDeviceInfo(info)
            let entries = try await fetchAllFiles()
            try epochGuard(flowEpoch)
            let existing = try await store.existingSourceIDs()
            reconcileRows(entries: entries, existingSourceIDs: existing)
            emitSnapshot()
            if autoSyncEnabled {
                let pending = snapshotValue.rows.filter { $0.status == .notDownloaded }.map(\.id)
                try await runDownloadQueue(pending, epoch: flowEpoch)
            } else {
                setState(.ready)
            }
        } catch is CancellationError {
            // 取消/断连/新会话作废旧流程：状态由发起方负责
        } catch let error as BuzzLinkError where error == .disconnected {
            // 断连由 handleClientState(.disconnected) 驱动重连
        } catch {
            guard flowEpoch == epoch else { return }
            logger.error("会话流程失败：\(error.localizedDescription)")
            setState(.ready)
            emitEvent(.syncFailed(error.localizedDescription))
        }
    }

    private func startDownloadFlow(ids: [String]) {
        sessionTask?.cancel()
        let flowEpoch = epoch
        sessionTask = Task { [weak self] in
            guard let self else { return }
            await self.runDownloadFlow(ids: ids, epoch: flowEpoch)
        }
    }

    private func runDownloadFlow(ids: [String], epoch flowEpoch: Int) async {
        sessionRunning = true
        defer { sessionRunning = false }
        do {
            try await runDownloadQueue(ids, epoch: flowEpoch)
        } catch is CancellationError {
            // cancelSync 已置态
        } catch let error as BuzzLinkError where error == .disconnected {
            // 断连处理器接管
        } catch {
            guard flowEpoch == epoch else { return }
            logger.error("下载队列失败：\(error.localizedDescription)")
            setState(.ready)
            emitEvent(.syncFailed(error.localizedDescription))
        }
    }

    /// 分页拉全设备文件列表（§8.2：每页最多 16 条）
    private func fetchAllFiles() async throws -> [BuzzLinkFileEntry] {
        var all: [BuzzLinkFileEntry] = []
        var offset = 0
        while true {
            let page = try await client.listFiles(offset: offset)
            all.append(contentsOf: page.files)
            offset += page.files.count
            if all.count >= page.total || page.files.isEmpty { break }
        }
        return all
    }

    /// diff：库内已有的标 verified；失败红标跨会话保留（人工重试）；其余未下载
    private func reconcileRows(entries: [BuzzLinkFileEntry], existingSourceIDs: Set<String>) {
        snapshotValue.rows = entries.map { entry in
            let status: DeviceFileRow.Status
            if existingSourceIDs.contains(entry.id) {
                status = .verified
            } else if let old = snapshotValue.rows.first(where: { $0.id == entry.id }),
                      case .failed(let reason) = old.status {
                status = .failed(reason)
            } else {
                status = .notDownloaded
            }
            return DeviceFileRow(id: entry.id, durationMs: entry.durationMs, size: entry.size, status: status)
        }
    }

    // MARK: - 私有：下载队列

    private func runDownloadQueue(_ ids: [String], epoch flowEpoch: Int) async throws {
        guard !ids.isEmpty else {
            setState(.ready)
            return
        }
        setState(.syncing)
        let totalBytes = ids.reduce(0) { total, id in
            total + (snapshotValue.rows.first { $0.id == id }?.size ?? 0)
        }
        let box = ProgressBox()
        var qs = QueueState(totalFiles: ids.count, totalBytes: totalBytes, box: box)
        queueState = qs
        let pump = startProgressPump()
        defer {
            pump.cancel()
            // 仅清理本队列的进度态（新会话可能已建立自己的）
            if queueState?.box === box {
                queueState = nil
                setProgress(nil)
            }
        }
        var succeeded = 0
        var failureStreak = 0
        var aborted = false

        for fileID in ids {
            try epochGuard(flowEpoch)
            guard let entry = snapshotValue.rows.first(where: { $0.id == fileID }) else { continue }
            qs.currentFileID = fileID
            qs.currentFileTotalBytes = entry.size
            qs.currentReceived = 0
            queueState = qs
            setRowStatus(fileID, .downloading(fraction: 0))
            do {
                // 元数据尽力获取（失败用协议默认值，不阻塞下载）
                let fileInfo = try? await client.fileInfo(id: fileID)
                try epochGuard(flowEpoch)
                let partial = try await store.preparePartial(for: fileID)
                let result = try await client.download(fileID: fileID) { progress in
                    box.record(progress)
                }
                try epochGuard(flowEpoch)
                try result.data.write(to: partial)
                let meta = makeMeta(fileID: fileID, entry: entry, result: result, fileInfo: fileInfo)
                _ = try await store.commitPartial(from: partial, meta: meta)
                qs.completedBytes += entry.size
                qs.completedFiles += 1
                // 间隙清理：迟到的末块进度不回灌已完成文件
                qs.currentFileID = nil
                qs.currentReceived = 0
                queueState = qs
                succeeded += 1
                failureStreak = 0
                setRowStatus(fileID, .verified)
            } catch let error as CancellationError {
                // 取消路径由 cancelSync 统一置态；残留 .partial 即时清理
                // （cancelSync 等待 session 结束后才返回，与本清理无竞态）
                try? await store.discardPartial(for: fileID)
                throw error
            } catch let error as BuzzLinkError where error == .disconnected {
                // 行态"未完成"由断连处理器统一标记；残留 .partial 由重下覆盖或下次连接清理
                // （不在此清理：与重连后的新会话存在竞态）
                throw error
            } catch {
                try? await store.discardPartial(for: fileID)
                failureStreak += 1
                logger.error("下载 \(fileID) 失败（连续第 \(failureStreak) 个）：\(error.localizedDescription)")
                setRowStatus(fileID, .failed(error.localizedDescription))
                if failureStreak >= 3 {
                    aborted = true
                    break
                }
            }
        }

        if aborted {
            // 连续失败：中断队列，卡片级提示（ux §4）；行内红标保留，人工重试
            setState(.failed("连续 \(failureStreak) 个文件同步失败，可能存在链路问题"))
            emitEvent(.syncFailed("连续 \(failureStreak) 个文件同步失败，可能存在链路问题"))
            return
        }
        setProgress(makeProgress(qs))   // 终态满格：UI 进度条到 100% 再消失
        setState(.ready)
        if succeeded > 0 {
            emitEvent(.syncCompleted(newFiles: succeeded))
        }
    }

    // MARK: - 私有：断连与自动重连

    private func handleClientState(_ state: BuzzLinkClientState) {
        guard state == .disconnected else { return }
        epoch &+= 1
        reconnectTask?.cancel()
        sessionTask?.cancel()
        stopKeepalive()
        queueState = nil
        setProgress(nil)
        markDownloadingRowsIncomplete()
        if suppressAutoReconnect {
            suppressAutoReconnect = false
            setState(.idle)
            return
        }
        guard hasEverBeenReady else {
            setState(.idle)
            return
        }
        setState(.reconnecting)
        emitEvent(.disconnected)
        startReconnect()
    }

    private func startReconnect() {
        let delays = config.reconnectDelays
        reconnectTask = Task { [weak self] in
            guard let self else { return }
            for delay in delays {
                try? await Task.sleep(for: .milliseconds(max(1, Int(delay * 1000))))
                if Task.isCancelled { return }
                if await self.tryReconnectOnce() { return }
            }
            await self.reconnectDidExhaust()
        }
    }

    private func tryReconnectOnce() async -> Bool {
        do {
            try await client.connect()
        } catch {
            logger.debug("自动重连失败：\(error.localizedDescription)")
            return false
        }
        startSessionFlow()
        return true
    }

    private func reconnectDidExhaust() {
        hasEverBeenReady = false
        setState(.idle)
        emitEvent(.reconnectExhausted)
    }

    // MARK: - 私有：空闲保活（§6.1）

    private func startKeepalive() {
        guard keepaliveTask == nil else { return }
        let interval = config.keepaliveInterval
        keepaliveTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(max(1, Int(interval * 1000))))
                guard !Task.isCancelled else { return }
                await self?.keepaliveTick()
            }
        }
    }

    private func stopKeepalive() {
        keepaliveTask?.cancel()
        keepaliveTask = nil
    }

    private func keepaliveTick() async {
        guard snapshotValue.state == .ready else { return }
        do {
            try await client.ping()
        } catch is CancellationError {
            // keepalive 任务自身被取消
        } catch {
            logger.info("保活 PING 失败（\(error.localizedDescription)），按断连处理")
            await client.disconnect()
        }
    }

    // MARK: - 私有：进度聚合

    private func startProgressPump() -> Task<Void, Never> {
        let intervalMilliseconds = max(20, Int(config.progressThrottleInterval * 1000))
        return Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(intervalMilliseconds))
                guard !Task.isCancelled, let self else { return }
                await self.progressTick()
            }
        }
    }

    private func progressTick() {
        guard var qs = queueState else { return }
        let now = Self.now()
        var rowChanged = false
        if let latest = qs.box.take(),
           latest.fileID == qs.currentFileID,
           latest.receivedBytes != qs.currentReceived {
            // 重试趟 received 回落：按当前趟覆盖式取值（不跨趟累加）
            qs.currentReceived = latest.receivedBytes
            qs.currentFileTotalBytes = latest.totalBytes
            rowChanged = updateRowFraction(latest)
        }
        qs.estimator.addSample(t: now, cumulativeBytes: qs.completedBytes + qs.currentReceived)
        // 心跳：无新数据时按 1s 节拍刷新（速率/ETA 随窗口衰减保持诚实）
        let heartbeatDue = now - qs.lastProgressEmitAt >= 1
        if rowChanged || heartbeatDue {
            qs.lastProgressEmitAt = now
            queueState = qs
            setProgress(makeProgress(qs))
        } else {
            queueState = qs
        }
    }

    /// 行进度更新（仅行仍处于下载中时；迟到的末块进度不回灌已完成行）
    private func updateRowFraction(_ latest: BuzzLinkDownloadProgress) -> Bool {
        guard let index = snapshotValue.rows.firstIndex(where: { $0.id == latest.fileID }),
              case .downloading = snapshotValue.rows[index].status else { return false }
        let total = max(1, latest.totalBytes)
        snapshotValue.rows[index].status = .downloading(
            fraction: min(1, Double(latest.receivedBytes) / Double(total))
        )
        return true
    }

    private func makeProgress(_ qs: QueueState) -> SyncProgress {
        let receivedTotal = qs.completedBytes + qs.currentReceived
        let overall = qs.totalBytes > 0 ? min(1, Double(receivedTotal) / Double(qs.totalBytes)) : 0
        let fileFraction = qs.currentFileTotalBytes > 0
            ? min(1, Double(qs.currentReceived) / Double(qs.currentFileTotalBytes))
            : 0
        let bytesPerSecond = qs.estimator.bytesPerSecond
        let remaining = max(0, qs.totalBytes - receivedTotal)
        let eta = bytesPerSecond > 0.5 ? TimeInterval(Double(remaining) / bytesPerSecond) : nil
        return SyncProgress(
            completedFiles: qs.completedFiles,
            totalFiles: qs.totalFiles,
            currentFileID: qs.currentFileID,
            fileFraction: fileFraction,
            overallFraction: overall,
            bytesPerSecond: bytesPerSecond,
            etaSeconds: eta
        )
    }

    // MARK: - 私有：快照与事件

    private func setState(_ newState: SyncState) {
        snapshotValue.state = newState
        if newState == .ready { startKeepalive() }
        emitSnapshot()
    }

    private func setProgress(_ progress: SyncProgress?) {
        snapshotValue.progress = progress
        emitSnapshot()
    }

    private func setDeviceInfo(_ info: BuzzLinkDeviceInfo) {
        snapshotValue.deviceInfo = info
        emitSnapshot()
    }

    private func setRowStatus(_ fileID: String, _ status: DeviceFileRow.Status) {
        guard let index = snapshotValue.rows.firstIndex(where: { $0.id == fileID }) else { return }
        snapshotValue.rows[index].status = status
        emitSnapshot()
    }

    private func markDownloadingRowsIncomplete() {
        for index in snapshotValue.rows.indices where snapshotValue.rows[index].status.isDownloading {
            snapshotValue.rows[index].status = .incomplete
        }
        emitSnapshot()
    }

    private func emitSnapshot() {
        snapshotValue.isStorageNearlyFull = Self.computeStorageNearlyFull(snapshotValue.deviceInfo)
        guard snapshotValue != lastEmittedSnapshot else { return }
        lastEmittedSnapshot = snapshotValue
        snapshotContinuation.yield(snapshotValue)
    }

    private func emitEvent(_ event: SyncEvent) {
        eventContinuation.yield(event)
    }

    // MARK: - 私有：杂项

    private var isFailed: Bool {
        if case .failed = snapshotValue.state { return true }
        return false
    }

    private func epochGuard(_ flowEpoch: Int) throws {
        if flowEpoch != epoch { throw CancellationError() }
    }

    private static func computeStorageNearlyFull(_ info: BuzzLinkDeviceInfo?) -> Bool {
        guard let info, info.storageTotal > 0 else { return false }
        return info.storageFree * 10 < info.storageTotal
    }

    private func makeMeta(
        fileID: String,
        entry: DeviceFileRow,
        result: BuzzLinkDownloadResult,
        fileInfo: BuzzLinkFileInfo?
    ) -> RecordingMeta {
        let recordedAt = RecordingMeta.parseRecordedAt(fromFileID: fileID)
        return RecordingMeta(
            schemaVersion: RecordingMeta.currentSchemaVersion,
            id: "",  // 由 commitPartial 按分配的 recording-id 覆写
            title: RecordingMeta.defaultTitle(for: fileID, recordedAt: recordedAt),
            source: RecordingMeta.Source(
                fileID: fileID,
                deviceName: snapshotValue.deviceInfo?.deviceName ?? "",
                deviceModel: snapshotValue.deviceInfo?.model ?? "",
                fwVersion: snapshotValue.deviceInfo?.fwVersion ?? ""
            ),
            durationMs: fileInfo?.durationMs ?? entry.durationMs,
            sizeBytes: result.size,
            crc16: result.crc16,
            format: fileInfo?.format ?? "ogg-opus",
            sampleRate: fileInfo?.sampleRate ?? 16_000,
            channels: fileInfo?.channels ?? 1,
            recordedAt: recordedAt,
            importedAt: Date(),
            audio: RecordingMeta.AudioFiles(ogg: "audio.ogg", m4a: nil),
            transcribe: RecordingMeta.PipelineStatus(status: "none"),
            summary: RecordingMeta.PipelineStatus(status: "none")
        )
    }

    private static func now() -> TimeInterval {
        ProcessInfo.processInfo.systemUptime
    }
}

// MARK: - 队列进度态（仅 actor 访问）

private struct QueueState {
    var totalFiles: Int
    var completedFiles = 0
    var totalBytes: Int
    var completedBytes = 0
    var currentFileID: String?
    var currentFileTotalBytes = 0
    var currentReceived = 0
    var estimator = RateEstimator()
    var lastProgressEmitAt: TimeInterval = 0
    /// 进度盒（身份比较用：清理时防止误删新会话的进度态）
    let box: ProgressBox
}

/// 进度盒：client 进度回调（`@Sendable` 非隔离）写入，actor 侧节流泵读取。
private final class ProgressBox: @unchecked Sendable {
    private let lock = NSLock()
    private var latest: BuzzLinkDownloadProgress?

    func record(_ progress: BuzzLinkDownloadProgress) {
        lock.withLock { latest = progress }
    }

    func take() -> BuzzLinkDownloadProgress? {
        lock.withLock { latest }
    }
}
