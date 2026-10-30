import Foundation

/// 故障注入配置（TESTING.md §5 故障注入矩阵）。
///
/// 全部为"关 = 合法行为"：默认值下设备严格按 BUZZLINK.md 应答，
/// 开启某项只在对应故障路径生效，不改变合法路径行为。
public struct SimFaults: Equatable, Sendable {
    /// 随机丢块概率（0 = 关闭；矩阵典型值 0.02）。
    /// 命中的块不发送但 seq 照常递增，客户端最终收不齐 → 文件 CRC 裁决（§7）。
    public var dropChunkProbability: Double
    /// 推流限速（字节/秒；nil = 不限速，如 1024 即矩阵的 1 KB/s）。
    /// 节奏由 BLE 壳层执行，引擎只携带配置。
    public var throttleBytesPerSecond: Int?
    /// 推流中途静默（模拟断连）：推到约 30% 后停止一切响应 `silentDuration` 秒。
    /// BLE 外设无法主动断开 Central，以"链路冻结"模拟；真断连路径请直接退出 Sim App。
    public var silentMidDownload: Bool
    /// 静默时长（秒），须大于客户端 chunk 间隔超时（§3.4：10 s）
    public var silentDuration: TimeInterval
    /// 末帧 CRC 错误：最后一个 DL_CHUNK 编码后翻转 CRC 字段
    public var corruptFinalChunkCRC: Bool
    /// 空文件列表：LIST 恒返回空，FILE_INFO/DL_BEGIN 一律 not_found
    public var emptyFileList: Bool
    /// 文件库追加异常文件名条目（``SimFile/hostileEntries()``）
    public var hostileFileNames: Bool
    /// 存储将满：INFO 上报剩余 5%（>90% 占用，触发客户端同步前警告条）
    public var storageNearlyFull: Bool
    /// 忽略 TIME_SET（v0.2 §8.4）：不响应 0x42——模拟旧固件 v0.1 对未知 type
    /// 的忽略行为（§10 兼容语义），用于客户端对时降级路径测试
    public var ignoreTimeSet: Bool

    public init(
        dropChunkProbability: Double = 0,
        throttleBytesPerSecond: Int? = nil,
        silentMidDownload: Bool = false,
        silentDuration: TimeInterval = 12,
        corruptFinalChunkCRC: Bool = false,
        emptyFileList: Bool = false,
        hostileFileNames: Bool = false,
        storageNearlyFull: Bool = false,
        ignoreTimeSet: Bool = false
    ) {
        self.dropChunkProbability = dropChunkProbability
        self.throttleBytesPerSecond = throttleBytesPerSecond
        self.silentMidDownload = silentMidDownload
        self.silentDuration = silentDuration
        self.corruptFinalChunkCRC = corruptFinalChunkCRC
        self.emptyFileList = emptyFileList
        self.hostileFileNames = hostileFileNames
        self.storageNearlyFull = storageNearlyFull
        self.ignoreTimeSet = ignoreTimeSet
    }
}

/// SplitMix64 伪随机数发生器（种子可注入，故障注入路径测试确定性用）。
public struct SplitMix64: Sendable {
    private var state: UInt64

    public init(seed: UInt64) {
        state = seed
    }

    public mutating func next() -> UInt64 {
        state &+= 0x9E37_79B9_7F4A_7C15
        var z = state
        z = (z ^ (z >> 30)) &* 0xBF58_4576_1CE4_E5B9
        z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
        return z ^ (z >> 31)
    }

    /// [0, 1) 均匀分布（53 位精度）
    public mutating func nextProbability() -> Double {
        Double(next() >> 11) / Double(1 << 53)
    }
}
