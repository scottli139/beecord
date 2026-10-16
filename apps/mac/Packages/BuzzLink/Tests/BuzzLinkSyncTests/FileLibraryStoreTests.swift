import BuzzLinkCore
import BuzzLinkSync
import Foundation
import XCTest

/// FileLibraryStore 单测：目录名安全化、原子入库、幂等、冲突后缀、残留清理、坏 meta 容忍。
final class FileLibraryStoreTests: XCTestCase {

    private func makeStore() throws -> (FileLibraryStore, URL) {
        let root = SyncTestSupport.makeTempRoot()
        return (FileLibraryStore(rootURL: root), root)
    }

    // MARK: - 目录名安全化

    func testSanitizedRecordingID() {
        let sanitize = FileLibraryStore.sanitizedRecordingID
        // 非法字符映射为 _
        XCTAssertEqual(sanitize("REC_2026/06:06*?"), "REC_2026_06_06__")
        XCTAssertEqual(sanitize("REC:2026"), "REC_2026")
        XCTAssertEqual(sanitize("a/b\\c:d*e?f\"g<h>i|j"), "a_b_c_d_e_f_g_h_i_j")
        // 中文保留
        XCTAssertEqual(sanitize("REC_超长文件名截断测ABCD"), "REC_超长文件名截断测ABCD")
        // 正常 id 原样
        XCTAssertEqual(sanitize("REC_20260101_120000"), "REC_20260101_120000")
        // 首字符点/空格替换
        XCTAssertEqual(sanitize(".hidden"), "_hidden")
        XCTAssertEqual(sanitize(" lead"), "_lead")
        // 空与全非法回退
        XCTAssertEqual(sanitize(""), "recording")
        XCTAssertEqual(sanitize("/"), "_")
        // 截断 48 字符
        let long = String(repeating: "x", count: 100)
        XCTAssertEqual(sanitize(long).count, 48)
        // 控制字符
        XCTAssertEqual(sanitize("a\0b\n"), "a_b_")
    }

    // MARK: - 入库与查重

    func testCommitWritesAudioAndMeta() async throws {
        let (store, root) = try makeStore()
        defer { try? FileManager.default.removeItem(at: root) }

        let content = Data(repeating: 0x5A, count: 1_000)
        let partial = try store.preparePartial(for: "REC_20260101_120000")
        try content.write(to: partial)
        var meta = SyncTestSupport.makeMeta(
            fileID: "REC_20260101_120000", size: content.count, crc16: CRC16.ccittFalse(content), durationMs: 5_000
        )
        meta.recordedAt = Date(timeIntervalSince1970: 1_767_225_600)  // 整秒（iso8601 无损往返）

        let recordingID = try store.commitPartial(from: partial, meta: meta)
        XCTAssertEqual(recordingID, "REC_20260101_120000")

        let dir = store.libraryURL.appendingPathComponent(recordingID)
        XCTAssertEqual(try Data(contentsOf: dir.appendingPathComponent("audio.ogg")), content)
        let decoded = try SyncTestSupport.decodeMeta(at: dir.appendingPathComponent("meta.json"))
        XCTAssertEqual(decoded.id, recordingID, "meta.id 由库分配覆写")
        XCTAssertEqual(decoded.source.fileID, "REC_20260101_120000")
        XCTAssertEqual(decoded.sizeBytes, 1_000)
        XCTAssertEqual(decoded.recordedAt, Date(timeIntervalSince1970: 1_767_225_600), "iso8601 日期应无损往返")
        XCTAssertEqual(decoded.importedAt, Date(timeIntervalSince1970: 1_760_000_000))
        XCTAssertEqual(decoded.transcribe.status, "none")
        XCTAssertNil(decoded.audio.m4a)

        XCTAssertEqual(try store.existingSourceIDs(), ["REC_20260101_120000"])
        // commit 后 partial 已 move 走
        XCTAssertFalse(FileManager.default.fileExists(atPath: partial.path))
    }

    func testCommitSameSourceIdempotent() async throws {
        let (store, root) = try makeStore()
        defer { try? FileManager.default.removeItem(at: root) }

        // 同一设备文件 id 重复入库：复用目录、内容替换、不产生副本
        for byte in [UInt8(0x11), 0x22] {
            let content = Data(repeating: byte, count: 500)
            let partial = try store.preparePartial(for: "REC_20260101_120000")
            try content.write(to: partial)
            let meta = SyncTestSupport.makeMeta(
                fileID: "REC_20260101_120000", size: 500, crc16: CRC16.ccittFalse(content), durationMs: 1_000
            )
            let id = try store.commitPartial(from: partial, meta: meta)
            XCTAssertEqual(id, "REC_20260101_120000")
        }
        XCTAssertEqual(try SyncTestSupport.libraryDirectoryNames(root: root), ["REC_20260101_120000"])
        let audio = try Data(contentsOf: store.libraryURL
            .appendingPathComponent("REC_20260101_120000").appendingPathComponent("audio.ogg"))
        XCTAssertEqual(audio, Data(repeating: 0x22, count: 500), "后写覆盖先写")
    }

    func testCommitConflictSuffixOnlyForDifferentSource() async throws {
        let (store, root) = try makeStore()
        defer { try? FileManager.default.removeItem(at: root) }

        // 两个不同设备文件 id 安全化后同名 → 冲突加 -2 后缀
        let first = Data(repeating: 0x01, count: 100)
        var partial = try store.preparePartial(for: "REC/2026")
        try first.write(to: partial)
        _ = try store.commitPartial(
            from: partial,
            meta: SyncTestSupport.makeMeta(fileID: "REC/2026", size: 100, crc16: CRC16.ccittFalse(first), durationMs: 1_000)
        )

        let second = Data(repeating: 0x02, count: 200)
        partial = try store.preparePartial(for: "REC:2026")
        try second.write(to: partial)
        let secondID = try store.commitPartial(
            from: partial,
            meta: SyncTestSupport.makeMeta(fileID: "REC:2026", size: 200, crc16: CRC16.ccittFalse(second), durationMs: 1_000)
        )

        XCTAssertEqual(secondID, "REC_2026-2", "异源同安全名冲突应加短后缀")
        XCTAssertEqual(try SyncTestSupport.libraryDirectoryNames(root: root), ["REC_2026", "REC_2026-2"])
        // 两边 meta 各保留原始文件 id
        let meta1 = try SyncTestSupport.decodeMeta(at: store.libraryURL
            .appendingPathComponent("REC_2026").appendingPathComponent("meta.json"))
        let meta2 = try SyncTestSupport.decodeMeta(at: store.libraryURL
            .appendingPathComponent("REC_2026-2").appendingPathComponent("meta.json"))
        XCTAssertEqual(meta1.source.fileID, "REC/2026")
        XCTAssertEqual(meta2.source.fileID, "REC:2026")
        XCTAssertEqual(meta2.id, "REC_2026-2")
        XCTAssertEqual(try store.existingSourceIDs(), ["REC/2026", "REC:2026"])
    }

    // MARK: - 残留清理

    func testPrepareAndDiscardPartial() async throws {
        let (store, root) = try makeStore()
        defer { try? FileManager.default.removeItem(at: root) }

        let partial = try store.preparePartial(for: "REC_20260101_120000")
        XCTAssertTrue(FileManager.default.fileExists(atPath: partial.path))
        XCTAssertEqual(partial.lastPathComponent, "REC_20260101_120000.ogg")
        XCTAssertTrue(partial.path.contains("/Library/.partial/"))

        // 重复 prepare 清掉旧残留
        try Data(repeating: 0xFF, count: 10).write(to: partial)
        let fresh = try store.preparePartial(for: "REC_20260101_120000")
        XCTAssertEqual(try Data(contentsOf: fresh).count, 0)

        try store.discardPartial(for: "REC_20260101_120000")
        XCTAssertFalse(FileManager.default.fileExists(atPath: fresh.path))
        // 不存在时幂等
        try store.discardPartial(for: "REC_29990101_000000")
    }

    func testPurgeStalePartials() async throws {
        let (store, root) = try makeStore()
        defer { try? FileManager.default.removeItem(at: root) }

        // 目录不存在时清理为无操作
        try store.purgeStalePartials()

        let first = try store.preparePartial(for: "REC_20260101_120000")
        let second = try store.preparePartial(for: "REC_20260102_093000")
        try Data(repeating: 0x1, count: 10).write(to: first)
        try Data(repeating: 0x2, count: 10).write(to: second)
        XCTAssertEqual(SyncTestSupport.partialFileNames(root: root).count, 2)

        try store.purgeStalePartials()
        XCTAssertTrue(SyncTestSupport.partialFileNames(root: root).isEmpty)
    }

    // MARK: - 坏 meta 容忍

    func testExistingSourceIDsToleratesCorruptMeta() async throws {
        let (store, root) = try makeStore()
        defer { try? FileManager.default.removeItem(at: root) }

        // 一个正常
        let content = Data(repeating: 0x33, count: 100)
        let partial = try store.preparePartial(for: "REC_20260101_120000")
        try content.write(to: partial)
        _ = try store.commitPartial(
            from: partial,
            meta: SyncTestSupport.makeMeta(
                fileID: "REC_20260101_120000", size: 100, crc16: CRC16.ccittFalse(content), durationMs: 1_000
            )
        )
        // 一个坏 meta 目录 + 一个无 meta 目录
        let corruptDir = store.libraryURL.appendingPathComponent("REC_CORRUPT", isDirectory: true)
        try FileManager.default.createDirectory(at: corruptDir, withIntermediateDirectories: true)
        try Data("not json".utf8).write(to: corruptDir.appendingPathComponent("meta.json"))
        try FileManager.default.createDirectory(
            at: store.libraryURL.appendingPathComponent("REC_NOMETA", isDirectory: true),
            withIntermediateDirectories: true
        )

        XCTAssertEqual(try store.existingSourceIDs(), ["REC_20260101_120000"], "坏/缺 meta 跳过不炸")
    }
}
