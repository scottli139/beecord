import AppKit
import BuzzLinkAudio
import BuzzLinkSync
import Foundation
import os

/// 录音库视图模型（T6 Phase B）：消费 LibraryCatalog 变更流驱动列表；
/// 转码任务串行队列（入库即转 + 存量补转最小接线，完整验证 Phase D）；
/// 重命名/手动录制时间/删除/显示于 Finder 意图。
///
/// 线程模型：@MainActor；catalog 为 actor，changes 为 Sendable 值流。
@MainActor
@Observable
final class LibraryViewModel {

    /// 转码态（行内徽标；详情播放器占位联动）
    enum TranscodeState: Equatable {
        case pending
        case running
        case failed(String)
        case ready
    }

    /// 录音条目（meta + 转码态）
    struct Item: Identifiable, Equatable {
        let meta: RecordingMeta
        var transcode: TranscodeState
        var id: String { meta.id }
    }

    private(set) var items: [Item] = []
    private(set) var selectedID: String?
    private(set) var isScanning = false

    // MARK: - 播放器（Phase C：仅 m4a 已就绪条目）
    private(set) var playerState: PlayerEngine.State = .idle
    private(set) var playerProgress = PlayerEngine.Progress(position: 0, duration: 0, fraction: 0, isPlaying: false)
    private(set) var waveform: [WaveformExtractor.Column] = []
    private(set) var playerRate: Float = 1
    private var playerEngine: PlayerEngine?
    nonisolated private let playerConsumption = ConsumptionHandle()

    let libraryRoot: URL

    private let appModel: AppModel
    private let store: FileLibraryStore
    private let catalog: LibraryCatalog
    private let transcoder = Transcoder()
    private let logger = Logger(subsystem: "com.beeuc.beecord.studio", category: "library")

    /// 流消费句柄：nonisolated，deinit 直接取消（@Observable 不允许 nonisolated 可变存储）
    nonisolated private let catalogConsumption = ConsumptionHandle()
    private var transcodeTask: Task<Void, Never>?
    private var transcodeQueue: [String] = []
    private var cancelledIDs: Set<String> = []
    /// 每次重新入库都换代，阻止同 ID 的旧转码回写新录音。
    private var transcodeGenerations: [String: UUID] = [:]

    init(appModel: AppModel, settings: SettingsModel) {
        self.appModel = appModel
        self.libraryRoot = LibraryRootResolver.resolve(settings: settings, appModel: appModel)
        self.store = FileLibraryStore(rootURL: libraryRoot)
        self.catalog = LibraryCatalog(store: store)

        catalogConsumption.add(Task { [weak self] in
            guard let self else { return }
            for await change in self.catalog.changes {
                await self.applyCatalogChange(change)
            }
        })
        // 同步完成广播：入库即转不依赖库页可见（手动下载完成时人通常在设备页；
        // 仅视图 onChange 会在库页未出现时漏派发，见 CI run 39040395432 复盘）
        catalogConsumption.add(Task { [weak self] in
            guard let self else { return }
            for await _ in self.appModel.syncCompletedStream {
                self.refresh()
            }
        })
        // 启动：全量扫描（存量录音进入列表；缺 m4a 的由变更事件触发补转）
        isScanning = true
        Task { [weak self] in
            guard let self else { return }
            await self.catalog.scanAll()
            self.isScanning = false
        }
    }

    deinit {
        catalogConsumption.cancelAll()
    }

    // MARK: - 派生状态

    /// 按录制时间新→旧（nil recordedAt 排最后，importedAt 兜底）
    var sortedItems: [Item] {
        items.sorted { lhs, rhs in
            let l = lhs.meta.recordedAt?.timeIntervalSince1970 ?? lhs.meta.importedAt.timeIntervalSince1970 - 1e12
            let r = rhs.meta.recordedAt?.timeIntervalSince1970 ?? rhs.meta.importedAt.timeIntervalSince1970 - 1e12
            if l != r { return l > r }
            return lhs.meta.id > rhs.meta.id
        }
    }

    var selectedItem: Item? {
        sortedItems.first { $0.id == selectedID }
    }

    var isEmpty: Bool { items.isEmpty }

    // MARK: - 意图方法

    func select(_ id: String) {
        selectedID = id
        Task { await preparePlayer() }
    }

    // MARK: - 播放器意图

    func togglePlay() async {
        await playerEngine?.togglePlay()
    }

    func skip(by delta: TimeInterval) async {
        await playerEngine?.skip(by: delta)
    }

    /// 倍速循环 1 → 1.5 → 2
    func cycleRate() async {
        let next: Float = playerRate == 1 ? 1.5 : (playerRate == 1.5 ? 2 : 1)
        playerRate = next
        await playerEngine?.setRate(next)
    }

    func seek(fraction: Double) async {
        guard playerProgress.duration > 0 else { return }
        await playerEngine?.seek(to: fraction * playerProgress.duration)
    }

    private func ensureEngine() -> PlayerEngine {
        if let engine = playerEngine { return engine }
        let engine = PlayerEngine(backend: AVAudioEngineBackend(), progressInterval: 0.1)
        playerEngine = engine
        playerConsumption.add(Task { [weak self] in
            guard let self else { return }
            for await progress in engine.progress {
                await self.applyPlayerProgress(progress)
            }
        })
        return engine
    }

    private func applyPlayerProgress(_ progress: PlayerEngine.Progress) async {
        playerProgress = progress
        playerState = progress.isPlaying ? .playing : (progress.duration > 0 ? .paused : .idle)
    }

    /// 选中项转码就绪则加载音频与波形；否则停止。
    private func preparePlayer() async {
        guard let item = selectedItem, item.transcode == .ready else {
            stopPlayer()
            return
        }
        let engine = ensureEngine()
        let m4aURL = store.libraryURL.appendingPathComponent(item.id).appendingPathComponent("audio.m4a")
        await engine.load(url: m4aURL)
        // 波形（缓存命中即返；失败不阻塞播放）
        waveform = []
        Task {
            let peaks = try? await WaveformExtractor.peaks(m4aURL: m4aURL)
            if selectedID == item.id {
                waveform = peaks ?? []
            }
        }
    }

    private func stopPlayer() {
        Task {
            await playerEngine?.stop()
            playerProgress = PlayerEngine.Progress(position: 0, duration: 0, fraction: 0, isPlaying: false)
            waveform = []
        }
    }

    func selectRecent() {
        if let first = sortedItems.first {
            selectedID = first.id
        }
    }

    /// 重命名：只改 meta.title（目录名=recording-id 不动，保幂等键）；空标题忽略。
    func rename(id: String, title: String) {
        let trimmed = title.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty,
              let index = items.firstIndex(where: { $0.id == id }),
              items[index].meta.title != trimmed else { return }
        var meta = items[index].meta
        meta.title = trimmed
        Task {
            do {
                try store.updateMeta(meta)
                await catalog.applyLocalUpdate(meta)
            } catch {
                appModel.showToast(ToastMessage(kind: .error, text: "重命名失败：\(error.localizedDescription)"))
            }
        }
    }

    func updateRecordedAt(id: String, date: Date) {
        guard let index = items.firstIndex(where: { $0.id == id }) else { return }
        var meta = items[index].meta
        let components = Calendar.current.dateComponents(
            [.year, .month, .day, .hour, .minute],
            from: date
        )
        let normalizedDate = Calendar.current.date(from: components) ?? date
        let utcOffsetMin = TimeZone.current.secondsFromGMT(for: normalizedDate) / 60
        meta.applyManualRecordedAt(normalizedDate, utcOffsetMin: utcOffsetMin)
        Task {
            do {
                try store.updateMeta(meta)
                await catalog.applyLocalUpdate(meta)
                appModel.showToast(ToastMessage(kind: .info, text: "已保存手动录制时间"))
            } catch {
                appModel.showToast(ToastMessage(kind: .error, text: "保存录制时间失败：\(error.localizedDescription)"))
            }
        }
    }

    /// 删除整目录 + catalog 即时移除 + 取消在飞转码。
    func deleteRecording(id: String) {
        // 标记取消：在飞/排队转码结果不再回写（转码自身会自然收敛）
        cancelledIDs.insert(id)
        transcodeGenerations.removeValue(forKey: id)
        transcodeQueue.removeAll { $0 == id }
        Task {
            try? store.deleteRecording(id: id)
            await catalog.applyLocalRemoval(id: id)
            if selectedID == id { selectedID = nil }
            appModel.showToast(ToastMessage(kind: .info, text: "已删除"))
        }
    }

    /// 转码失败重试（人工触发）。
    func retryTranscode(id: String) {
        guard let index = items.firstIndex(where: { $0.id == id }),
              case .failed = items[index].transcode else { return }
        cancelledIDs.remove(id)
        items[index].transcode = .pending
        enqueueTranscode(id: id)
    }

    /// 刷新（页面 appear / 同步完成后调用）：增量对账 + 补转派发。
    func refresh() {
        Task { [catalog] in
            await catalog.refresh()
        }
    }

    /// 显示于 Finder。
    func revealInFinder(id: String) {
        let url = store.libraryURL.appendingPathComponent(id, isDirectory: true)
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }

    // MARK: - catalog 变更消费

    private func applyCatalogChange(_ change: LibraryCatalog.Change) async {
        switch change {
        case .added(let meta):
            // added 是新的入库生命周期：解除旧删除标记，并隔离同 ID 的在飞旧任务。
            cancelledIDs.remove(meta.id)
            transcodeGenerations[meta.id] = UUID()
            transcodeQueue.removeAll { $0 == meta.id }
            items.removeAll { $0.id == meta.id }
            await upsert(meta: meta)
        case .updated(let meta):
            if transcodeGenerations[meta.id] == nil {
                transcodeGenerations[meta.id] = UUID()
            }
            await upsert(meta: meta)
        case .removed(let id):
            transcodeGenerations.removeValue(forKey: id)
            items.removeAll { $0.id == id }
            if selectedID == id {
                selectedID = nil
                stopPlayer()
            }
        }
    }

    private func upsert(meta: RecordingMeta) async {
        let needsTranscode = meta.audio.m4a == nil
        if let index = items.firstIndex(where: { $0.id == meta.id }) {
            let old = items[index]
            items[index] = Item(
                meta: meta,
                transcode: needsTranscode ? old.transcode : .ready
            )
        } else {
            items.append(Item(meta: meta, transcode: needsTranscode ? .pending : .ready))
        }
        if needsTranscode, !cancelledIDs.contains(meta.id) {
            let state = items.first(where: { $0.id == meta.id })?.transcode ?? .pending
            if state == .pending { enqueueTranscode(id: meta.id) }
        } else if selectedID == meta.id, meta.audio.m4a != nil {
            Task { await preparePlayer() }
        }
    }

    // MARK: - 转码队列（串行）

    private func enqueueTranscode(id: String) {
        guard !transcodeQueue.contains(id) else { return }
        transcodeQueue.append(id)
        pumpTranscode()
    }

    private func pumpTranscode() {
        guard transcodeTask == nil, let id = transcodeQueue.first else { return }
        transcodeQueue.removeFirst()
        transcodeTask = Task { [weak self] in
            defer {
                self?.transcodeTask = nil
                self?.pumpTranscode()
            }
            await self?.runTranscode(id: id)
        }
    }

    private func runTranscode(id: String) async {
        guard !cancelledIDs.contains(id),
              let generation = transcodeGenerations[id],
              let index = items.firstIndex(where: { $0.id == id }) else { return }
        items[index].transcode = .running
        let directory = store.libraryURL.appendingPathComponent(id)
        let oggURL = directory.appendingPathComponent("audio.ogg")
        let m4aURL = directory.appendingPathComponent("audio.m4a")
        // 每次执行独占产物，旧任务即使在重下后才结束，也无法覆盖新任务的 audio.m4a。
        let temporaryURL = directory.appendingPathComponent(".transcode-\(UUID().uuidString).m4a")
        defer { try? FileManager.default.removeItem(at: temporaryURL) }

        let existingSize = (try? FileManager.default.attributesOfItem(atPath: m4aURL.path)[.size] as? Int) ?? 0
        let result: Result<Transcoder.Outcome, Transcoder.TranscodeError>
        if existingSize > 0 {
            result = .success(.alreadyExisted)
        } else {
            result = await transcoder.transcribe(oggURL: oggURL, m4aURL: temporaryURL)
        }

        // 任务期间可能已删除并以同 ID 重新入库：旧代次不得发布产物或修改新 UI。
        guard transcodeGenerations[id] == generation,
              !cancelledIDs.contains(id),
              let current = items.firstIndex(where: { $0.id == id }) else { return }
        switch result {
        case .success(.transcribed), .success(.alreadyExisted):
            if case .success(.transcribed) = result {
                do {
                    try FileManager.default.moveItem(at: temporaryURL, to: m4aURL)
                } catch {
                    items[current].transcode = .failed(error.localizedDescription)
                    logger.error("发布转码产物 \(id) 失败：\(error.localizedDescription)")
                    return
                }
            }
            var meta = items[current].meta
            meta.audio.m4a = "audio.m4a"
            try? store.updateMeta(meta)
            await catalog.applyLocalUpdate(meta)
            guard transcodeGenerations[id] == generation,
                  !cancelledIDs.contains(id),
                  let latest = items.firstIndex(where: { $0.id == id }),
                  items[latest].transcode == .running else { return }
            items[latest].transcode = .ready
        case .success(.cancelled):
            if items[current].transcode == .running { items[current].transcode = .pending }
        case .failure(let error):
            items[current].transcode = .failed(error.localizedDescription)
            logger.error("转码 \(id, privacy: .public) 失败：\(error.localizedDescription, privacy: .public)")
        }
    }
}
