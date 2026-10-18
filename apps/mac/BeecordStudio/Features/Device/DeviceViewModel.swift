import AppKit
import BuzzLink
import BuzzLinkCore
import BuzzLinkSync
import Foundation
import os

/// 设备页视图模型（T5 阶段 C）：装配 transport/client/SyncEngine，消费快照与
/// 事件流驱动 UI；权限面罩只在 BLE 模式出现（loopback 完全不触 CB 栈）。
///
/// 线程模型：@MainActor；两个流消费 Task 随 VM 生命周期（deinit 取消），
/// engine 为 actor，快照/事件均为 Sendable 值。
@MainActor
@Observable
final class DeviceViewModel {

    /// 权限面罩原因（蓝牙未授权 / 蓝牙未开启）。
    enum PermissionReason: Equatable {
        case bluetoothDenied
        case bluetoothOff

        var title: String {
            switch self {
            case .bluetoothDenied: "没有蓝牙访问权限"
            case .bluetoothOff: "蓝牙未开启"
            }
        }

        var message: String {
            switch self {
            case .bluetoothDenied: "Beecord Studio 需要蓝牙权限才能连接录音设备。"
            case .bluetoothOff: "请在系统设置中打开蓝牙后重试。"
            }
        }
    }

    /// 引擎快照（流驱动；初始值不回放，由 VM 自建同形初值）
    private(set) var snapshot: SyncSnapshot
    /// 文件表勾选的行 id（仅可选择行）
    private(set) var selectedIDs: Set<String> = []
    /// 非 nil = 显示蓝牙权限面罩
    private(set) var permissionOverlay: PermissionReason?

    let settings: SettingsModel

    /// T7 帧旁听数据源（协议控制台订阅其 events；装配点 = init 的 tap 包装）
    let frameTap: FrameTapTransport

    private let client: BuzzLinkClient

    private let appModel: AppModel
    nonisolated private let engine: SyncEngine
    private let logger = Logger(subsystem: "com.beeuc.beecord.studio", category: "device")

    /// 流消费句柄：nonisolated，VM 释放时统一取消（@Observable 不允许 nonisolated 可变存储）
    nonisolated private let consumption: ConsumptionHandle

    private var didAutoConnect = false
    /// 首轮自动连接（失败时 loopback 模式不弹错误 toast，避免日常空转噪音）
    private var isAutoConnectAttempt = false

    init(appModel: AppModel, settings: SettingsModel) {
        self.appModel = appModel
        self.settings = settings
        self.snapshot = SyncSnapshot(
            state: .idle, rows: [], progress: nil, deviceInfo: nil, isStorageNearlyFull: false
        )
        // transport 工厂：loopback（默认）/ BLE（P0 单设备直连）
        let baseTransport: any BuzzLinkTransport
        switch TransportMode(rawValue: settings.transportMode) ?? .loopback {
        case .ble:
            baseTransport = BLETransport()
        case .loopback:
            baseTransport = LoopbackTransport()
        }
        // T7 编辑点①：包 tap 装饰器——控制台帧旁听数据源；同步路径仅 copy + 增量重组
        // （FrameTapTransport 头注释），BLE/loopback 行为一致。
        let tap = FrameTapTransport(wrapping: baseTransport)
        self.frameTap = tap
        let client = BuzzLinkClient(transport: tap)
        self.client = client
        let store = FileLibraryStore(rootURL: Self.makeLibraryRoot(settings: settings, appModel: appModel))
        let config = SyncConfig(autoSyncEnabled: settings.autoSyncEnabled)
        let engine = SyncEngine(client: client, store: store, config: config)
        self.engine = engine
        let consumption = ConsumptionHandle()
        self.consumption = consumption
        consumption.add(makeSnapshotTask(engine: engine))
        consumption.add(makeEventTask(engine: engine))
        LaunchTrace.mark("deviceVM.init")
    }

    deinit {
        consumption.cancelAll()
    }

    /// 快照流消费（nonisolated：闭包内经 await 回主 actor）
    private nonisolated func makeSnapshotTask(engine: SyncEngine) -> Task<Void, Never> {
        Task { [weak self] in
            for await latest in engine.snapshots {
                guard let self else { return }
                await self.applySnapshot(latest)
            }
        }
    }

    /// 事件流消费（toast 触发）
    private nonisolated func makeEventTask(engine: SyncEngine) -> Task<Void, Never> {
        Task { [weak self] in
            for await event in engine.events {
                guard let self else { return }
                await self.handleEvent(event)
            }
        }
    }

    // MARK: - 派生状态

    /// 是否已连接（含连接后流程中的各态）
    var isConnected: Bool {
        switch snapshot.state {
        case .idle, .connecting:
            return false
        default:
            return true
        }
    }

    /// 可选择行（未下载 / 失败 / 未完成；已校验与下载中不可选）
    var selectableRows: [DeviceFileRow] {
        snapshot.rows.filter { row in
            switch row.status {
            case .notDownloaded, .failed, .incomplete:
                return true
            case .verified, .downloading:
                return false
            }
        }
    }

    /// 是否全选（所有可选择行均被勾选）
    var isSelectAll: Bool {
        let selectable = selectableRows
        return !selectable.isEmpty && selectable.allSatisfy { selectedIDs.contains($0.id) }
    }

    /// 下载所选按钮标题
    var downloadButtonTitle: String {
        "下载所选（\(selectedIDs.count)）"
    }

    // MARK: - 意图方法

    /// 首次出现自动连接一次；失败停留空态（首连失败不自动重试，避免空态抖动）。
    func onAppear() {
        LaunchTrace.mark("deviceView.onAppear didAutoConnect=\(didAutoConnect)")
        guard !didAutoConnect else { return }
        didAutoConnect = true
        isAutoConnectAttempt = true
        connectTapped()
    }

    /// 连接（=扫描一体，P0 单设备直连）。BLE 模式先查授权；被拒出面罩。
    func connectTapped() {
        if isBLEMode {
            switch BuzzLinkBluetooth.currentStatus() {
            case .denied:
                permissionOverlay = .bluetoothDenied
                return
            case .notDetermined, .poweredOff, .ready:
                permissionOverlay = nil
            }
        } else {
            permissionOverlay = nil
        }
        Task { [engine] in
            await engine.connect()
            LaunchTrace.mark("engine.connect returned")
        }
    }

    func disconnectTapped() {
        Task { [engine] in
            await engine.disconnect()
        }
    }

    func refreshTapped() {
        Task { [engine] in
            await engine.refresh()
        }
    }

    func toggleSelectAll() {
        let selectable = selectableRows
        if isSelectAll {
            selectedIDs.subtract(selectable.map(\.id))
        } else {
            selectedIDs.formUnion(selectable.map(\.id))
        }
    }

    func toggleSelection(_ fileID: String) {
        guard selectableRows.contains(where: { $0.id == fileID }) else { return }
        if selectedIDs.contains(fileID) {
            selectedIDs.remove(fileID)
        } else {
            selectedIDs.insert(fileID)
        }
    }

    func downloadSelectedTapped() {
        let ids = selectedIDs.sorted()
        guard !ids.isEmpty else { return }
        selectedIDs.removeAll()
        Task { [engine] in
            await engine.downloadSelected(ids)
        }
    }

    func cancelTapped() {
        Task { [engine] in
            await engine.cancelSync()
        }
    }

    func retryTapped(_ fileID: String) {
        Task { [engine] in
            await engine.retry(fileID)
        }
    }

    /// 自动同步开关：持久化 + 引擎即时生效（空闲且有待下载时立即触发一轮）。
    func setAutoSync(_ enabled: Bool) {
        settings.setAutoSyncEnabled(enabled)
        Task { [engine] in
            await engine.setAutoSyncEnabled(enabled)
        }
    }

    /// 打开系统蓝牙设置面板。
    func openBluetoothSettings() {
        guard let url = URL(string: "x-apple.systempreferences:com.apple.Bluetooth") else { return }
        NSWorkspace.shared.open(url)
    }

    // MARK: - T7 协议控制台

    /// 控制台原始命令通道：经 tap 包装的 client.sendRaw（session 分配见回调；
    /// 门控/超时语义同高层命令）。ConsoleViewModel 专用。
    func sendRawConsoleCommand(
        _ frame: BuzzLinkFrame,
        awaitResponse: Bool,
        allocatedSession: @Sendable (UInt8) -> Void = { _ in }
    ) async throws -> BuzzLinkFrame? {
        try await client.sendRaw(frame, awaitResponse: awaitResponse, allocatedSession: allocatedSession)
    }

    // MARK: - 流消费

    private func applySnapshot(_ latest: SyncSnapshot) {
        snapshot = latest
        // 勾选集跟随行集合收缩；行完成/下载中后移出勾选
        let selectable = Set(selectableRows.map(\.id))
        selectedIDs.formIntersection(selectable)
    }

    private func handleEvent(_ event: SyncEvent) {
        switch event {
        case .syncCompleted(let newFiles):
            isAutoConnectAttempt = false
            appModel.markSyncCompleted()
            appModel.showToast(ToastMessage(kind: .info, text: "同步完成，新增 \(newFiles) 个录音"))
        case .syncFailed(let message):
            // BLE 模式授权就绪却连不上 → 推断蓝牙未开启/不支持，出面罩（不弹 toast）
            if isBLEMode, BuzzLinkBluetooth.currentStatus() == .ready {
                permissionOverlay = .bluetoothOff
            } else if !(isLoopbackMode && isAutoConnectAttempt) {
                appModel.showToast(ToastMessage(kind: .error, text: message))
            }
            isAutoConnectAttempt = false
        case .disconnected:
            isAutoConnectAttempt = false
            appModel.showToast(ToastMessage(kind: .info, text: "连接已断开，正在自动重连…"))
        case .reconnectExhausted:
            appModel.showToast(ToastMessage(kind: .error, text: "自动重连失败，请检查设备后手动重试"))
        }
    }

    // MARK: - 杂项

    private var isBLEMode: Bool {
        TransportMode(rawValue: settings.transportMode) == .ble
    }

    private var isLoopbackMode: Bool {
        !isBLEMode
    }

    /// 库根解析委托公共 resolver（行为不变）。
    private static func makeLibraryRoot(settings: SettingsModel, appModel: AppModel) -> URL {
        LibraryRootResolver.resolve(settings: settings, appModel: appModel)
    }
}

/// 流消费任务句柄：跨隔离域取消用（Task 存储为 Sendable 值，取消幂等）。
final class ConsumptionHandle: @unchecked Sendable {
    private let lock = NSLock()
    private var tasks: [Task<Void, Never>] = []

    func add(_ task: Task<Void, Never>) {
        lock.withLock { tasks.append(task) }
    }

    func cancelAll() {
        lock.withLock {
            for task in tasks { task.cancel() }
            tasks.removeAll()
        }
    }
}
