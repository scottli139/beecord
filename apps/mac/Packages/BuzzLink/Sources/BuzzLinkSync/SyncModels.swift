import BuzzLink
import Foundation

/// 同步引擎状态机（studio-tech-design §5.2 流程；行级状态见 ``DeviceFileRow/Status``）。
public enum SyncState: Sendable, Equatable {
    /// 未连接
    case idle
    /// 扫描/连接中
    case connecting
    /// 已连接空闲
    case ready
    /// 拉取设备信息/文件列表并与库 diff 中
    case listing
    /// 队列下载中（``SyncSnapshot/progress`` 有效）
    case syncing
    /// 取消收尾中（等待在飞下载合作式中断）
    case cancelling
    /// 断连自动重连中（仅"曾经 ready 的会话"掉线触发）
    case reconnecting
    /// 卡片级失败（连续多个文件失败，疑似链路问题，ux §4）
    case failed(String)
}

/// 设备文件行（设备页文件表；AX 行锚点用稳定业务 id = 设备文件 id）。
public struct DeviceFileRow: Sendable, Equatable, Identifiable {
    /// 设备文件 id
    public var id: String
    public var durationMs: UInt32
    public var size: Int
    public var status: Status

    /// 行状态（DESIGN_SYSTEM §2 状态徽标语义）
    public enum Status: Sendable, Equatable {
        /// 未下载
        case notDownloaded
        /// 下载中（当前趟进度 0-1）
        case downloading(fraction: Double)
        /// ✓ 已校验（含库内已有）
        case verified
        /// 失败（CRC/重试耗尽等）：行内红标 + 重试入口
        case failed(String)
        /// 断连未完成：重连后可重新下载
        case incomplete
    }

    public init(id: String, durationMs: UInt32, size: Int, status: Status) {
        self.id = id
        self.durationMs = durationMs
        self.size = size
        self.status = status
    }
}

public extension DeviceFileRow.Status {
    /// 是否下载中
    var isDownloading: Bool {
        if case .downloading = self { return true }
        return false
    }
}

/// 同步进度（字节加权聚合，供 UI/AX 断言）。
public struct SyncProgress: Sendable, Equatable {
    /// 已完成文件数（x）
    public var completedFiles: Int
    /// 队列文件总数（y）
    public var totalFiles: Int
    /// 当前下载中文件 id；队列间隙为 nil
    public var currentFileID: String?
    /// 当前文件进度 0-1
    public var fileFraction: Double
    /// 整体进度 0-1（已完成字节 + 当前文件已收）/ 队列总字节
    public var overallFraction: Double
    /// 滑动窗口速率（字节/秒）
    public var bytesPerSecond: Double
    /// 预计剩余秒数；速率≈0 时为 nil
    public var etaSeconds: TimeInterval?

    public init(
        completedFiles: Int,
        totalFiles: Int,
        currentFileID: String?,
        fileFraction: Double,
        overallFraction: Double,
        bytesPerSecond: Double,
        etaSeconds: TimeInterval?
    ) {
        self.completedFiles = completedFiles
        self.totalFiles = totalFiles
        self.currentFileID = currentFileID
        self.fileFraction = fileFraction
        self.overallFraction = overallFraction
        self.bytesPerSecond = bytesPerSecond
        self.etaSeconds = etaSeconds
    }
}

/// 引擎快照（state/rows/progress/deviceInfo 聚合；去重后发射）。
public struct SyncSnapshot: Sendable, Equatable {
    public var state: SyncState
    public var rows: [DeviceFileRow]
    /// 同步中进度；非下载态为 nil
    public var progress: SyncProgress?
    public var deviceInfo: BuzzLinkDeviceInfo?
    /// 设备存储占用 >90%（同步前警告条，ux §4）
    public var isStorageNearlyFull: Bool

    public init(
        state: SyncState,
        rows: [DeviceFileRow],
        progress: SyncProgress?,
        deviceInfo: BuzzLinkDeviceInfo?,
        isStorageNearlyFull: Bool
    ) {
        self.state = state
        self.rows = rows
        self.progress = progress
        self.deviceInfo = deviceInfo
        self.isStorageNearlyFull = isStorageNearlyFull
    }
}

/// 离散事件（toast 触发用；边沿语义，与快照流分离）。
public enum SyncEvent: Sendable, Equatable {
    /// 一轮同步完成（新入库文件数）
    case syncCompleted(newFiles: Int)
    /// 同步/连接失败（toast 文案）
    case syncFailed(String)
    /// 意外断连（自动重连已开始）
    case disconnected
    /// 自动重连退避耗尽（需人工介入）
    case reconnectExhausted
}

/// 同步引擎配置。
public struct SyncConfig: Sendable, Equatable {
    /// 自动同步：连接后 diff 出新文件即入队（ux 评审结论：默认开）
    public var autoSyncEnabled: Bool
    /// 自动重连退避序列（秒）；耗尽 → 放弃并回报
    public var reconnectDelays: [TimeInterval]
    /// 空闲保活间隔（秒；BUZZLINK §6.1 建议 15）；仅 `.ready` 空闲态发送
    public var keepaliveInterval: TimeInterval
    /// 进度快照节流间隔（秒）
    public var progressThrottleInterval: TimeInterval
    /// 取消收敛有界等待（秒）；超时兜底断开并自动重连
    public var cancelConvergenceTimeout: TimeInterval

    public init(
        autoSyncEnabled: Bool = true,
        reconnectDelays: [TimeInterval] = [1, 2, 4, 8, 10, 10, 10],
        keepaliveInterval: TimeInterval = 15,
        progressThrottleInterval: TimeInterval = 0.1,
        cancelConvergenceTimeout: TimeInterval = 3
    ) {
        self.autoSyncEnabled = autoSyncEnabled
        self.reconnectDelays = reconnectDelays
        self.keepaliveInterval = keepaliveInterval
        self.progressThrottleInterval = progressThrottleInterval
        self.cancelConvergenceTimeout = cancelConvergenceTimeout
    }

    /// 默认配置
    public static let `default` = SyncConfig()
}
