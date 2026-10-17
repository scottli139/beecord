import BuzzLinkSync
import XCTest

/// LibraryCatalog：全量扫描 / 增量刷新 / 删除与更新事件 / manifest 持久化。
final class LibraryCatalogTests: XCTestCase {

    private var root: URL!
    private var store: FileLibraryStore!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("catalog-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        store = FileLibraryStore(rootURL: root)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: root)
    }

    private func makeMeta(fileID: String) -> RecordingMeta {
        RecordingMeta(
            schemaVersion: RecordingMeta.currentSchemaVersion,
            id: "",
            title: fileID,
            source: RecordingMeta.Source(fileID: fileID, deviceName: "BCD-DEMO", deviceModel: "sim", fwVersion: "0.1.0"),
            durationMs: 1_000,
            sizeBytes: 64,
            crc16: 1,
            format: "ogg-opus",
            sampleRate: 16_000,
            channels: 1,
            recordedAt: nil,
            importedAt: Date(),
            audio: RecordingMeta.AudioFiles(ogg: "audio.ogg", m4a: nil),
            transcribe: RecordingMeta.PipelineStatus(status: "none"),
            summary: RecordingMeta.PipelineStatus(status: "none")
        )
    }

    private func commitOne(_ fileID: String) async throws -> String {
        let partial = try store.preparePartial(for: fileID)
        try Data(repeating: 1, count: 32).write(to: partial)
        return try store.commitPartial(from: partial, meta: makeMeta(fileID: fileID))
    }

    /// 迭代器盒：跨越 @Sendable 任务边界（测试辅助；串行消费，无并发）
    private final class IteratorBox: @unchecked Sendable {
        var iterator: AsyncStream<LibraryCatalog.Change>.Iterator
        init(_ iterator: AsyncStream<LibraryCatalog.Change>.Iterator) {
            self.iterator = iterator
        }
    }

    /// 单项有界读取（与超时竞速；裸 `iterator.next()` 在无事件时会永久挂起）
    private func nextChange(
        _ box: IteratorBox,
        timeout: TimeInterval = 2
    ) async -> LibraryCatalog.Change? {
        await withTaskGroup(of: LibraryCatalog.Change?.self) { group in
            group.addTask { await box.iterator.next() }
            group.addTask {
                try? await Task.sleep(for: .milliseconds(Int(timeout * 1000)))
                return nil
            }
            let result = await group.next() ?? nil
            group.cancelAll()
            return result
        }
    }

    private func collectChanges(
        _ stream: AsyncStream<LibraryCatalog.Change>,
        until predicate: ([LibraryCatalog.Change]) -> Bool,
        timeout: TimeInterval = 5
    ) async -> [LibraryCatalog.Change] {
        var collected: [LibraryCatalog.Change] = []
        let box = IteratorBox(stream.makeAsyncIterator())
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline, !predicate(collected) {
            guard let change = await nextChange(box, timeout: 1) else { break }
            collected.append(change)
        }
        return collected
    }

    func testScanAllEmitsAddedAndPopulatesSnapshot() async throws {
        let catalog = LibraryCatalog(store: store)
        let idA = try await commitOne("REC_20260101_120000")
        let idB = try await commitOne("REC_20260102_093000")
        await catalog.scanAll()
        let changes = await collectChanges(catalog.changes) { $0.count >= 2 }
        XCTAssertEqual(changes.count, 2)
        if case .added(let meta) = changes[0] {
            XCTAssertEqual(meta.id, idA)
        } else {
            XCTFail("首事件应为 added")
        }
        let snapshot = await catalog.recordings
        XCTAssertEqual(Set(snapshot.map(\.id)), [idA, idB])
    }

    func testRefreshEmitsRemovedOnDelete() async throws {
        let catalog = LibraryCatalog(store: store)
        let id = try await commitOne("REC_20260101_120000")
        await catalog.scanAll()
        _ = await collectChanges(catalog.changes) { $0.count >= 1 }
        try store.deleteRecording(id: id)
        await catalog.refresh()
        let changes = await collectChanges(catalog.changes) { $0.contains(.removed(id: id)) }
        XCTAssertTrue(changes.contains(.removed(id: id)))
        let snapshotAfter = await catalog.recordings
        XCTAssertTrue(snapshotAfter.isEmpty)
    }

    func testRefreshEmitsUpdatedOnMetaRewrite() async throws {
        let catalog = LibraryCatalog(store: store)
        let id = try await commitOne("REC_20260101_120000")
        await catalog.scanAll()
        _ = await collectChanges(catalog.changes) { $0.count >= 1 }
        // mtime 粒度为秒：先等到下一秒再改写，保证增量可感知
        try await Task.sleep(for: .milliseconds(1100))
        let loadedMeta = try store.recording(id: id)
        var meta = try XCTUnwrap(loadedMeta)
        meta.title = "改名"
        try store.updateMeta(meta)
        await catalog.refresh()
        let changes = await collectChanges(catalog.changes) { $0.contains(.updated(meta: meta)) }
        XCTAssertTrue(changes.contains(.updated(meta: meta)))
    }

    func testManifestPersistsAcrossCatalogInstances() async throws {
        let first = LibraryCatalog(store: store)
        let id = try await commitOne("REC_20260101_120000")
        await first.scanAll()
        _ = await collectChanges(first.changes) { $0.count >= 1 }
        // 新实例（模拟重启）：manifest 已落盘，快照直接可用
        let second = LibraryCatalog(store: store)
        await second.scanAll()
        let snapshot = await second.recordings
        XCTAssertEqual(snapshot.map(\.id), [id])
    }

    func testLocalUpdateBroadcastsWithoutRescan() async throws {
        let catalog = LibraryCatalog(store: store)
        let id = try await commitOne("REC_20260101_120000")
        await catalog.scanAll()
        _ = await collectChanges(catalog.changes) { $0.count >= 1 }
        let loadedMeta = try store.recording(id: id)
        var meta = try XCTUnwrap(loadedMeta)
        meta.title = "本地改名"
        try store.updateMeta(meta)
        await catalog.applyLocalUpdate(meta)
        let changes = await collectChanges(catalog.changes) { $0.contains(.updated(meta: meta)) }
        XCTAssertTrue(changes.contains(.updated(meta: meta)))
        let snapshotLocal = await catalog.recordings
        XCTAssertEqual(snapshotLocal.first?.title, "本地改名")
    }
}
