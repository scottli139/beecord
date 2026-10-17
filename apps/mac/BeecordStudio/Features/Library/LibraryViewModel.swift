import AppKit
import BuzzLinkSync
import BuzzLinkAudio
import Foundation
import os

/// 录音库视图模型（T6 Phase B）：消费 LibraryCatalog 变更流驱动列表；
/// 转码任务串行队列（入库即转 + 存量补转最小接线，完整验证 Phase D）；
/// 重命名/删除/显示于 Finder 意图。
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

    /// 删除整目录 + catalog 即时移除 + 取消在飞转码。
    func deleteRecording(id: String) {
        // 标记取消：在飞/排队转码结果不再回写（转码自身会自然收敛）
        cancelledIDs.insert(id)
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
        case .added(let meta), .updated(let meta):
            await upsert(meta: meta)
        case .removed(let id):
            items.removeAll { $0.id == id }
            if selectedID == id { selectedID = nil }
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
              let index = items.firstIndex(where: { $0.id == id }) else { return }
        items[index].transcode = .running
        let oggURL = store.libraryURL.appendingPathComponent(id).appendingPathComponent("audio.ogg")
        let m4aURL = store.libraryURL.appendingPathComponent(id).appendingPathComponent("audio.m4a")
        let result = await transcoder.transcribe(oggURL: oggURL, m4aURL: m4aURL)
        // 任务期间模型可能已变化（删除/刷新）：以最新索引为准
        guard let current = items.firstIndex(where: { $0.id == id }) else { return }
        switch result {
        case .success(.transcribed), .success(.alreadyExisted):
            var meta = items[current].meta
            meta.audio.m4a = "audio.m4a"
            try? store.updateMeta(meta)
            await catalog.applyLocalUpdate(meta)
            if let latest = items.firstIndex(where: { $0.id == id }), items[latest].transcode == .running {
                items[latest].transcode = .ready
            }
        case .success(.cancelled):
            if items[current].transcode == .running { items[current].transcode = .pending }
        case .failure(let error):
            items[current].transcode = .failed(error.localizedDescription)
            logger.error("转码 \(id) 失败：\(error.localizedDescription)")
        }
    }
}
