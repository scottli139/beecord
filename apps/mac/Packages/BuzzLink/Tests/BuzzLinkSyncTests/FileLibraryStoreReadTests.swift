import BuzzLinkSync
import XCTest

/// LibraryStore read 侧扩展（T6 Phase A）：listRecordings / recording(id:) /
/// deleteRecording / updateMeta 的行为与原子性。
final class FileLibraryStoreReadTests: XCTestCase {

    private var root: URL!
    private var store: FileLibraryStore!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("store-read-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        store = FileLibraryStore(rootURL: root)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: root)
    }

    private func makeMeta(fileID: String, title: String = "标题") -> RecordingMeta {
        RecordingMeta(
            schemaVersion: RecordingMeta.currentSchemaVersion,
            id: "",
            title: title,
            source: RecordingMeta.Source(fileID: fileID, deviceName: "BCD-DEMO", deviceModel: "sim", fwVersion: "0.1.0"),
            durationMs: 5_000,
            sizeBytes: 128,
            crc16: 42,
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

    /// 造一条入库记录，返回分配的 recording-id
    private func commitOne(_ fileID: String, title: String = "标题") async throws -> String {
        let partial = try store.preparePartial(for: fileID)
        try Data(repeating: 1, count: 64).write(to: partial)
        return try store.commitPartial(from: partial, meta: makeMeta(fileID: fileID, title: title))
    }

    func testListRecordingsReturnsAllUnordered() async throws {
        _ = try await commitOne("REC_20260101_120000")
        _ = try await commitOne("REC_20260102_093000")
        let list = try store.listRecordings()
        XCTAssertEqual(list.count, 2)
        XCTAssertEqual(Set(list.map(\.source.fileID)), ["REC_20260101_120000", "REC_20260102_093000"])
    }

    func testListRecordingsSkipsCorruptMeta() async throws {
        let id = try await commitOne("REC_20260101_120000")
        // 破坏一个 meta.json
        let metaURL = root.appendingPathComponent("Library/\(id)/meta.json")
        try Data("not-json".utf8).write(to: metaURL)
        // 再建一条正常记录（绕过 list 校验路径，直接第二个文件）
        _ = try await commitOne("REC_20260102_093000")
        let list = try store.listRecordings()
        XCTAssertEqual(list.count, 1)
        XCTAssertEqual(list[0].source.fileID, "REC_20260102_093000")
    }

    func testRecordingByIDHitAndMiss() async throws {
        let id = try await commitOne("REC_20260101_120000")
        let hit = try store.recording(id: id)
        XCTAssertEqual(hit?.source.fileID, "REC_20260101_120000")
        let miss = try store.recording(id: "不存在")
        XCTAssertNil(miss)
    }

    func testDeleteRecordingIdempotent() async throws {
        let id = try await commitOne("REC_20260101_120000")
        try store.deleteRecording(id: id)
        let afterDelete = try store.recording(id: id)
        XCTAssertNil(afterDelete)
        // 二次删除无事发生
        try store.deleteRecording(id: id)
        // 其余记录不受影响
        _ = try await commitOne("REC_20260102_093000")
        let remaining = try store.listRecordings()
        XCTAssertEqual(remaining.count, 1)
    }

    func testUpdateMetaPersists() async throws {
        let id = try await commitOne("REC_20260101_120000", title: "旧标题")
        let loadedMeta = try store.recording(id: id)
        var meta = try XCTUnwrap(loadedMeta)
        meta.title = "新标题"
        try store.updateMeta(meta)
        let reloaded = try store.recording(id: id)
        XCTAssertEqual(reloaded?.title, "新标题")
    }

    func testUpdateMetaMissingDirectoryThrows() async throws {
        let meta = makeMeta(fileID: "REC_20990101_000000")
        await XCTAssertThrowsErrorAsync(try store.updateMeta(meta)) { error in
            guard case LibraryStoreError.writeFailed = error else {
                return XCTFail("期望 writeFailed，实际 \(error)")
            }
        }
    }
}

/// 异步断言辅助（XCTAssertThrowsError 无 async 版）
private func XCTAssertThrowsErrorAsync<T>(
    _ expression: @autoclosure () async throws -> T,
    _ message: @autoclosure () -> String = "",
    file: StaticString = #filePath,
    line: UInt = #line,
    _ errorHandler: (Error) -> Void = { _ in }
) async {
    do {
        _ = try await expression()
        XCTFail("期望抛出错误\(message())", file: file, line: line)
    } catch {
        errorHandler(error)
    }
}
