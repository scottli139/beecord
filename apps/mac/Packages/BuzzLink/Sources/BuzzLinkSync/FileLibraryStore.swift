import Foundation
import os

/// 录音库文件系统实现（studio-tech-design §5.3）。
///
/// 布局：`<root>/Library/<recording-id>/{audio.ogg, meta.json}`；下载中文件放
/// `<root>/Library/.partial/<safe>.ogg`。commit 为同卷 `moveItem`（原子 rename）
/// + `meta.json` 原子写。方法为同步实现（async 协议要求的非 async 见证）；
/// 目录枚举开销随库规模线性增长，启动对账/manifest 缓存属 T6 范围。
public struct FileLibraryStore: LibraryStore {

    /// 库根目录（默认由上层传 Application Support/Beecord；测试注入临时目录）
    public let rootURL: URL

    private let logger = Logger(subsystem: "com.beeuc.beecord.buzzlink", category: "library")

    public init(rootURL: URL) {
        self.rootURL = rootURL
    }

    // MARK: - 路径

    /// 库目录（`<root>/Library`）
    public var libraryURL: URL {
        rootURL.appendingPathComponent("Library", isDirectory: true)
    }

    /// 下载中临时目录（`<root>/Library/.partial`）
    public var partialDirectoryURL: URL {
        libraryURL.appendingPathComponent(".partial", isDirectory: true)
    }

    // MARK: - LibraryStore

    public func existingSourceIDs() throws -> Set<String> {
        let fileManager = FileManager.default
        guard let dirs = try? fileManager.contentsOfDirectory(at: libraryURL, includingPropertiesForKeys: nil) else {
            return []
        }
        var ids = Set<String>()
        for dir in dirs {
            let name = dir.lastPathComponent
            guard name != ".partial" else { continue }
            let metaURL = dir.appendingPathComponent("meta.json")
            guard fileManager.fileExists(atPath: metaURL.path) else { continue }
            do {
                ids.insert(try readMeta(in: dir).source.fileID)
            } catch {
                logger.error("跳过损坏的 meta.json（\(name)）：\(error.localizedDescription)")
            }
        }
        return ids
    }

    public func preparePartial(for fileID: String) throws -> URL {
        let fileManager = FileManager.default
        try fileManager.createDirectory(at: partialDirectoryURL, withIntermediateDirectories: true)
        let url = partialURL(for: fileID)
        if fileManager.fileExists(atPath: url.path) {
            try fileManager.removeItem(at: url)
        }
        guard fileManager.createFile(atPath: url.path, contents: nil) else {
            throw LibraryStoreError.writeFailed("无法创建临时文件：\(url.lastPathComponent)")
        }
        return url
    }

    public func commitPartial(from partial: URL, meta: RecordingMeta) throws -> String {
        let fileManager = FileManager.default
        try fileManager.createDirectory(at: libraryURL, withIntermediateDirectories: true)
        let base = Self.sanitizedRecordingID(for: meta.source.fileID)
        var suffix = 1
        while true {
            let candidate = suffix == 1 ? base : "\(base)-\(suffix)"
            let dir = libraryURL.appendingPathComponent(candidate, isDirectory: true)
            if fileManager.fileExists(atPath: dir.path) {
                // 同源幂等复用目录；异源冲突加短后缀
                if let existing = try? readMeta(in: dir), existing.source.fileID == meta.source.fileID {
                    try commitInto(directory: dir, recordingID: candidate, partial: partial, meta: meta)
                    return candidate
                }
                suffix += 1
                continue
            }
            try commitInto(directory: dir, recordingID: candidate, partial: partial, meta: meta)
            return candidate
        }
    }

    public func discardPartial(for fileID: String) throws {
        let url = partialURL(for: fileID)
        let fileManager = FileManager.default
        guard fileManager.fileExists(atPath: url.path) else { return }
        try fileManager.removeItem(at: url)
    }

    public func purgeStalePartials() throws {
        let fileManager = FileManager.default
        guard let files = try? fileManager.contentsOfDirectory(at: partialDirectoryURL, includingPropertiesForKeys: nil) else {
            return
        }
        for file in files {
            do {
                try fileManager.removeItem(at: file)
            } catch {
                logger.error("清理残留临时文件失败（\(file.lastPathComponent)）：\(error.localizedDescription)")
            }
        }
    }

    // MARK: - read 侧（T6 录音库）

    public func listRecordings() throws -> [RecordingMeta] {
        let fileManager = FileManager.default
        guard let dirs = try? fileManager.contentsOfDirectory(at: libraryURL, includingPropertiesForKeys: nil) else {
            return []
        }
        var result: [RecordingMeta] = []
        for dir in dirs {
            let name = dir.lastPathComponent
            guard name != ".partial" else { continue }
            guard fileManager.fileExists(atPath: dir.appendingPathComponent("meta.json").path) else { continue }
            do {
                result.append(try readMeta(in: dir))
            } catch {
                logger.error("跳过损坏的 meta.json（\(name)）：\(error.localizedDescription)")
            }
        }
        return result
    }

    public func recording(id: String) throws -> RecordingMeta? {
        let dir = libraryURL.appendingPathComponent(id, isDirectory: true)
        guard FileManager.default.fileExists(atPath: dir.appendingPathComponent("meta.json").path) else {
            return nil
        }
        return try readMeta(in: dir)
    }

    public func deleteRecording(id: String) throws {
        let dir = libraryURL.appendingPathComponent(id, isDirectory: true)
        guard FileManager.default.fileExists(atPath: dir.path) else { return }
        try FileManager.default.removeItem(at: dir)
    }

    public func updateMeta(_ meta: RecordingMeta) throws {
        let dir = libraryURL.appendingPathComponent(meta.id, isDirectory: true)
        guard FileManager.default.fileExists(atPath: dir.path) else {
            throw LibraryStoreError.writeFailed("目录不存在：\(meta.id)")
        }
        let data = try makeMetaData(meta)
        try data.write(to: dir.appendingPathComponent("meta.json"), options: .atomic)
    }

    // MARK: - 目录名安全化

    /// 由设备文件 id 生成安全的目录名：非法文件系统字符（`/:*?"<>|\`）与控制字符
    /// 替换为 `_`，首字符为 `.` 或空格时同样替换；按字符截断至 48；空结果回退
    /// 为 "recording"。
    public static func sanitizedRecordingID(for fileID: String) -> String {
        let invalid = CharacterSet(charactersIn: "/:*?\"<>|\\").union(.controlCharacters)
        var result = ""
        for character in fileID {
            let isInvalid = character.unicodeScalars.contains { invalid.contains($0) }
            if isInvalid || (result.isEmpty && (character == "." || character == " ")) {
                result.append("_")
            } else {
                result.append(character)
            }
        }
        result = String(result.prefix(48))
        return result.isEmpty ? "recording" : result
    }

    // MARK: - 私有

    private func partialURL(for fileID: String) -> URL {
        partialDirectoryURL.appendingPathComponent(Self.sanitizedRecordingID(for: fileID) + ".ogg")
    }

    private func commitInto(directory: URL, recordingID: String, partial: URL, meta: RecordingMeta) throws {
        let fileManager = FileManager.default
        try fileManager.createDirectory(at: directory, withIntermediateDirectories: true)
        let audioDestination = directory.appendingPathComponent("audio.ogg")
        if fileManager.fileExists(atPath: audioDestination.path) {
            try fileManager.removeItem(at: audioDestination)
        }
        // 同卷原子 rename（.partial 与 Library 同根，恒同卷）
        try fileManager.moveItem(at: partial, to: audioDestination)
        var finalMeta = meta
        finalMeta.id = recordingID
        let data = try makeMetaData(finalMeta)
        try data.write(to: directory.appendingPathComponent("meta.json"), options: .atomic)
    }

    private func makeMetaData(_ meta: RecordingMeta) throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        return try encoder.encode(meta)
    }

    private func readMeta(in directory: URL) throws -> RecordingMeta {
        let data = try Data(contentsOf: directory.appendingPathComponent("meta.json"))
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return try decoder.decode(RecordingMeta.self, from: data)
    }
}
