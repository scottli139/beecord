import Foundation

/// 客户端状态机（studio-tech-design §4.2：断连收敛到 `disconnected`，重连策略由上层决定）。
public enum BuzzLinkClientState: Sendable, Equatable {
    case idle
    case scanning
    case connecting
    case ready
    case downloading(fileID: String)
    case verifying(fileID: String)
    case disconnected
}

/// 设备信息（INFO，BUZZLINK.md §8.1）。
public struct BuzzLinkDeviceInfo: Sendable, Equatable {
    public var model: String
    public var fwVersion: String
    /// 协议版本号（对应 frame version）
    public var protocolVersion: Int
    public var deviceName: String
    /// 电量百分比（0–100）
    public var battery: Int
    public var isCharging: Bool
    /// 存储总容量（字节）
    public var storageTotal: UInt64
    /// 存储剩余（字节）
    public var storageFree: UInt64

    public init(
        model: String,
        fwVersion: String,
        protocolVersion: Int,
        deviceName: String,
        battery: Int,
        isCharging: Bool,
        storageTotal: UInt64,
        storageFree: UInt64
    ) {
        self.model = model
        self.fwVersion = fwVersion
        self.protocolVersion = protocolVersion
        self.deviceName = deviceName
        self.battery = battery
        self.isCharging = isCharging
        self.storageTotal = storageTotal
        self.storageFree = storageFree
    }
}

/// 文件列表页条目（LIST_RESP，BUZZLINK.md §8.2）。
public struct BuzzLinkFileEntry: Sendable, Equatable, Identifiable {
    public var id: String
    public var durationMs: UInt32
    public var size: Int

    public init(id: String, durationMs: UInt32, size: Int) {
        self.id = id
        self.durationMs = durationMs
        self.size = size
    }
}

/// 文件列表页（LIST_RESP，BUZZLINK.md §8.2）。
public struct BuzzLinkFileListPage: Sendable, Equatable {
    /// 设备文件总数（翻页依据）
    public var total: Int
    public var offset: Int
    public var files: [BuzzLinkFileEntry]

    public init(total: Int, offset: Int, files: [BuzzLinkFileEntry]) {
        self.total = total
        self.offset = offset
        self.files = files
    }
}

/// 文件元数据（FILE_INFO_RESP，BUZZLINK.md §8.3）。
public struct BuzzLinkFileInfo: Sendable, Equatable {
    public var id: String
    public var size: Int
    /// 整文件 CRC-16（十进制 0–65535）
    public var crc16: UInt16
    public var durationMs: UInt32
    public var format: String
    public var sampleRate: Int
    public var channels: Int

    public init(
        id: String,
        size: Int,
        crc16: UInt16,
        durationMs: UInt32,
        format: String,
        sampleRate: Int,
        channels: Int
    ) {
        self.id = id
        self.size = size
        self.crc16 = crc16
        self.durationMs = durationMs
        self.format = format
        self.sampleRate = sampleRate
        self.channels = channels
    }
}

/// 下载进度（单趟尝试内，receivedBytes 单调递增；重试时 attempt 递增并重新计数）。
public struct BuzzLinkDownloadProgress: Sendable, Equatable {
    public var fileID: String
    /// 当前第几趟尝试（从 1 开始）
    public var attempt: Int
    public var receivedBytes: Int
    public var totalBytes: Int

    public init(fileID: String, attempt: Int, receivedBytes: Int, totalBytes: Int) {
        self.fileID = fileID
        self.attempt = attempt
        self.receivedBytes = receivedBytes
        self.totalBytes = totalBytes
    }
}

/// 下载验收结果（§6.3：三处 CRC 全等后的完整文件）。
public struct BuzzLinkDownloadResult: Sendable, Equatable {
    public var fileID: String
    public var data: Data
    public var size: Int
    /// 本地计算的整文件 CRC-16（与 BEGIN/END 携带值全等）
    public var crc16: UInt16

    public init(fileID: String, data: Data, size: Int, crc16: UInt16) {
        self.fileID = fileID
        self.data = data
        self.size = size
        self.crc16 = crc16
    }
}
