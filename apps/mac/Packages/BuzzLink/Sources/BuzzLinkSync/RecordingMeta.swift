import BuzzLink
import Foundation

public enum RecordingTimeSource: String, Codable, Sendable, Equatable {
    case device
    case filename
    case manual
}

/// 录音元数据（`meta.json` 的 Codable schema，studio-tech-design §5.3）。
///
/// `id` 为 recording-id（= 库内目录名），由 LibraryStore 在入库时分配并覆写
/// （构造时可先置空串）；同步幂等键由 `sourceKeys` 计算，新记录优先使用 recordingID。
/// `transcribe`/`summary` 为 P1 占位（状态机 none/running/done/failed）。
public struct RecordingMeta: Codable, Sendable, Equatable {

    /// 当前 schema 版本
    public static let currentSchemaVersion = 2

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
    /// 起录时间（显式设备元数据优先；文件名仅作旧格式兜底）
    public var recordedAt: Date?
    public var recordedAtUtcOffsetMin: Int?
    public var recordedAtSource: RecordingTimeSource?
    public var recordingTimeQuality: BuzzLinkTimeQuality?
    /// 入库（同步完成）时间
    public var importedAt: Date
    public var audio: AudioFiles
    public var transcribe: PipelineStatus
    public var summary: PipelineStatus

    /// 来源设备
    public struct Source: Codable, Sendable, Equatable {
        /// 设备文件寻址 ID；旧格式去重还需结合设备身份
        public var fileID: String
        public var deviceName: String
        public var deviceModel: String
        public var fwVersion: String
        public var recordingID: String?
        public var deviceID: String?

        public init(
            fileID: String,
            deviceName: String,
            deviceModel: String,
            fwVersion: String,
            recordingID: String? = nil,
            deviceID: String? = nil
        ) {
            self.fileID = fileID
            self.deviceName = deviceName
            self.deviceModel = deviceModel
            self.fwVersion = fwVersion
            self.recordingID = recordingID
            self.deviceID = deviceID
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
        summary: PipelineStatus,
        recordedAtUtcOffsetMin: Int? = nil,
        recordedAtSource: RecordingTimeSource? = nil,
        recordingTimeQuality: BuzzLinkTimeQuality? = nil
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
        self.recordedAtUtcOffsetMin = recordedAtUtcOffsetMin
        self.recordedAtSource = recordedAtSource
        self.recordingTimeQuality = recordingTimeQuality
        self.importedAt = importedAt
        self.audio = audio
        self.transcribe = transcribe
        self.summary = summary
    }

    /// 旧格式文件名兜底：支持 REC_YYYYMMDD_HHMMSS、扩展名和同秒 _XXXX 后缀。
    /// 设备本地时间按本机时区解释；不符合旧命名约定时返回 nil。
    public static func parseRecordedAt(fromFileID fileID: String) -> Date? {
        guard fileID.hasPrefix("REC_") else { return nil }
        var stamp = String(fileID.dropFirst(4))
        if let dot = stamp.firstIndex(of: ".") {
            stamp = String(stamp[..<dot])
        }
        if stamp.count == 20, stamp.dropFirst(15).first == "_" {
            stamp = String(stamp.prefix(15))
        }
        guard stamp.count == 15 else { return nil }
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = .current
        formatter.isLenient = false
        formatter.dateFormat = "yyyyMMdd_HHmmss"
        return formatter.date(from: stamp)
    }

    /// 默认标题：起录时间可读文本；有原始 UTC 偏移时按该偏移显示。
    public static func defaultTitle(
        for fileID: String,
        recordedAt: Date?,
        utcOffsetMin: Int? = nil
    ) -> String {
        guard let recordedAt else { return fileID }
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        if let utcOffsetMin, let zone = TimeZone(secondsFromGMT: utcOffsetMin * 60) {
            formatter.timeZone = zone
        } else {
            formatter.timeZone = .current
        }
        formatter.dateFormat = "yyyy-MM-dd HH:mm"
        return formatter.string(from: recordedAt) + " 录音"
    }

    public static func sourceKeys(
        fileID: String,
        recordingID: String?,
        deviceID: String?,
        deviceName: String
    ) -> Set<String> {
        if let recordingID, !recordingID.isEmpty {
            return ["rid:\(recordingID.lowercased())"]
        }
        var keys: Set<String> = ["legacy:\(deviceName):\(fileID)"]
        if let deviceID, !deviceID.isEmpty {
            keys.insert("legacy:\(deviceID):\(fileID)")
        }
        return keys
    }

    public var sourceKeys: Set<String> {
        Self.sourceKeys(
            fileID: source.fileID,
            recordingID: source.recordingID,
            deviceID: source.deviceID,
            deviceName: source.deviceName
        )
    }

    /// 人工时间只改本地展示元数据；保持标题、来源键和音频身份不变。
    public mutating func applyManualRecordedAt(_ date: Date, utcOffsetMin: Int) {
        schemaVersion = Self.currentSchemaVersion
        recordedAt = date
        recordedAtUtcOffsetMin = utcOffsetMin
        recordedAtSource = .manual
        recordingTimeQuality = nil
    }
}
