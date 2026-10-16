import Foundation

/// 录音元数据（`meta.json` 的 Codable schema，studio-tech-design §5.3）。
///
/// `id` 为 recording-id（= 库内目录名），由 LibraryStore 在入库时分配并覆写
/// （构造时可先置空串）；`source.fileID` 是设备文件 id，为同步 diff 的幂等键。
/// `transcribe`/`summary` 为 P1 占位（状态机 none/running/done/failed）。
public struct RecordingMeta: Codable, Sendable, Equatable {

    /// 当前 schema 版本
    public static let currentSchemaVersion = 1

    public var schemaVersion: Int
    /// recording-id（库内目录名；入库时由 LibraryStore 分配覆写）
    public var id: String
    /// 标题（默认=起录时间文本；T6 可重命名）
    public var title: String
    public var source: Source
    public var durationMs: UInt32
    public var sizeBytes: Int
    /// 整文件 CRC-16（十进制 0–65535）
    public var crc16: UInt16
    /// 封装/编码格式（如 "ogg-opus"）
    public var format: String
    public var sampleRate: Int
    public var channels: Int
    /// 起录时间（从文件 id 解析；失败为 nil）
    public var recordedAt: Date?
    /// 入库（同步完成）时间
    public var importedAt: Date
    public var audio: AudioFiles
    public var transcribe: PipelineStatus
    public var summary: PipelineStatus

    /// 来源设备
    public struct Source: Codable, Sendable, Equatable {
        /// 设备文件 id（幂等键）
        public var fileID: String
        public var deviceName: String
        public var deviceModel: String
        public var fwVersion: String

        public init(fileID: String, deviceName: String, deviceModel: String, fwVersion: String) {
            self.fileID = fileID
            self.deviceName = deviceName
            self.deviceModel = deviceModel
            self.fwVersion = fwVersion
        }
    }

    /// 音频产物（库目录内相对路径）
    public struct AudioFiles: Codable, Sendable, Equatable {
        /// 原始文件（"audio.ogg"）
        public var ogg: String
        /// 播放用转码产物（T6 转码后回填 "audio.m4a"）
        public var m4a: String?

        public init(ogg: String, m4a: String? = nil) {
            self.ogg = ogg
            self.m4a = m4a
        }
    }

    /// 转写/纪要流水线状态（P1 占位）
    public struct PipelineStatus: Codable, Sendable, Equatable {
        /// none / running / done / failed
        public var status: String

        public init(status: String) {
            self.status = status
        }
    }

    public init(
        schemaVersion: Int,
        id: String,
        title: String,
        source: Source,
        durationMs: UInt32,
        sizeBytes: Int,
        crc16: UInt16,
        format: String,
        sampleRate: Int,
        channels: Int,
        recordedAt: Date?,
        importedAt: Date,
        audio: AudioFiles,
        transcribe: PipelineStatus,
        summary: PipelineStatus
    ) {
        self.schemaVersion = schemaVersion
        self.id = id
        self.title = title
        self.source = source
        self.durationMs = durationMs
        self.sizeBytes = sizeBytes
        self.crc16 = crc16
        self.format = format
        self.sampleRate = sampleRate
        self.channels = channels
        self.recordedAt = recordedAt
        self.importedAt = importedAt
        self.audio = audio
        self.transcribe = transcribe
        self.summary = summary
    }

    /// 从设备文件 id 解析起录时间（固件命名 `REC_YYYYMMDD_HHMMSS`，BUZZLINK §5.2）。
    /// 设备本地时间按本机时区解释；id 不符合命名约定时返回 nil。
    public static func parseRecordedAt(fromFileID fileID: String) -> Date? {
        guard fileID.hasPrefix("REC_") else { return nil }
        let stamp = String(fileID.dropFirst(4))
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = .current
        formatter.dateFormat = "yyyyMMdd_HHmmss"
        return formatter.date(from: stamp)
    }

    /// 默认标题：起录时间可读文本（"2026-01-01 12:00 录音"）；解析失败回退为文件 id
    public static func defaultTitle(for fileID: String, recordedAt: Date?) -> String {
        guard let recordedAt else { return fileID }
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = .current
        formatter.dateFormat = "yyyy-MM-dd HH:mm"
        return formatter.string(from: recordedAt) + " 录音"
    }
}
