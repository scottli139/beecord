import BuzzLink
import BuzzLinkCore
import BuzzLinkSimCore
import BuzzLinkSync
import XCTest

/// 自动同步与 diff 测试（连接 → 设备信息 → 列表 → 与库 diff → 队列化下载入库）。
final class SyncEngineAutoSyncTests: XCTestCase {

    private func sampleFiles() -> [SimFile] {
        [
            SyncTestSupport.makeFile(id: "REC_20260101_120000", durationMs: 5_000, size: 5 * 224, seed: 1),
            SyncTestSupport.makeFile(id: "REC_20260102_093000", durationMs: 30_000, size: 10 * 224, seed: 2),
            SyncTestSupport.makeFile(id: "REC_20260103_074500", durationMs: 120_000, size: 3 * 224, seed: 3)
        ]
    }

    // MARK: - 1. 自动同步全量入库

    func testAutoSyncDownloadsNewFilesAndWritesLibrary() async throws {
        let files = sampleFiles()
        let fixture = makeSyncFixture(files: files)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let snapshots = SnapshotCollector()
        let events = SyncEventCollector()
        let collectingSnapshots = collectSnapshots(of: fixture.engine, into: snapshots)
        let collectingEvents = collectEvents(of: fixture.engine, into: events)
        defer { collectingSnapshots.cancel(); collectingEvents.cancel() }

        await fixture.engine.connect()
        let done = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready
                && snapshot.rows.count == 3
                && snapshot.rows.allSatisfy { $0.status == .verified }
        }
        XCTAssertTrue(done, "自动同步应将 3 个文件全部入库并回 ready")

        // 库内容：逐字节一致 + meta 可查重
        let sourceIDs = try fixture.store.existingSourceIDs()
        XCTAssertEqual(sourceIDs, Set(files.map(\.id)))
        for file in files {
            let dir = fixture.store.libraryURL.appendingPathComponent(file.id)
            let audio = try Data(contentsOf: dir.appendingPathComponent("audio.ogg"))
            XCTAssertEqual(audio, file.data, "\(file.id) 内容须逐字节一致")
            let meta = try SyncTestSupport.decodeMeta(at: dir.appendingPathComponent("meta.json"))
            XCTAssertEqual(meta.schemaVersion, RecordingMeta.currentSchemaVersion)
            XCTAssertEqual(meta.id, file.id, "recording-id = 设备文件 id（无冲突时）")
            XCTAssertEqual(meta.source.fileID, file.id, "幂等键为设备文件 id")
            XCTAssertEqual(meta.sizeBytes, file.data.count)
            XCTAssertEqual(meta.crc16, CRC16.ccittFalse(file.data))
            XCTAssertEqual(meta.durationMs, file.durationMs, "durationMs 以 FILE_INFO/列表为准")
            XCTAssertEqual(meta.format, "ogg-opus")
            XCTAssertEqual(meta.sampleRate, 16_000)
            XCTAssertEqual(meta.channels, 1)
            XCTAssertNotNil(meta.recordedAt, "REC_YYYYMMDD_HHMMSS 应可解析起录时间")
            XCTAssertEqual(meta.audio.ogg, "audio.ogg")
            XCTAssertNil(meta.audio.m4a, "转码产物 T6 才回填")
            XCTAssertEqual(meta.transcribe.status, "none")
            XCTAssertEqual(meta.summary.status, "none")
            XCTAssertEqual(meta.source.deviceName, "BCD-DEMO")
        }
        // 设备信息已进入快照；存储未告警（Sim 默认剩余 39%）
        let finalSnapshot = await fixture.engine.snapshot
        XCTAssertEqual(finalSnapshot.deviceInfo?.deviceName, "BCD-DEMO")
        XCTAssertFalse(finalSnapshot.isStorageNearlyFull)
        // 完成后进度清空、partial 无残留
        XCTAssertNil(finalSnapshot.progress)
        XCTAssertTrue(SyncTestSupport.partialFileNames(root: fixture.root).isEmpty, ".partial 应无残留")
        // 完成事件
        let eventArrived = await pollUntil(timeout: 2) {
            events.values.contains { $0 == .syncCompleted(newFiles: 3) }
        }
        XCTAssertTrue(eventArrived, "应发 syncCompleted(3) 事件")
        await fixture.engine.disconnect()
    }

    // MARK: - 2. diff 跳过库内已有

    func testDiffSkipsExisting() async throws {
        let files = sampleFiles()
        let fixture = makeSyncFixture(files: files)
        defer { try? FileManager.default.removeItem(at: fixture.root) }

        // 预置：files[0] 已在库，内容为哨兵字节（验证不被重下覆盖）
        let existing = files[0]
        let sentinel = Data(repeating: 0xAB, count: existing.data.count)
        let partial = try fixture.store.preparePartial(for: existing.id)
        try sentinel.write(to: partial)
        let meta = SyncTestSupport.makeMeta(
            fileID: existing.id, size: sentinel.count, crc16: CRC16.ccittFalse(sentinel), durationMs: existing.durationMs
        )
        _ = try fixture.store.commitPartial(from: partial, meta: meta)

        await fixture.engine.connect()
        let done = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready
                && snapshot.rows.count == 3
                && snapshot.rows.allSatisfy { $0.status == .verified }
        }
        XCTAssertTrue(done)

        // 预置文件未被重下：哨兵内容不变
        let dir = fixture.store.libraryURL.appendingPathComponent(existing.id)
        let audio = try Data(contentsOf: dir.appendingPathComponent("audio.ogg"))
        XCTAssertEqual(audio, sentinel, "已在库文件不应重新下载")
        // 库内共 3 个目录（2 个新入库 + 1 个预置）
        let dirs = try SyncTestSupport.libraryDirectoryNames(root: fixture.root)
        XCTAssertEqual(dirs.count, 3)
        XCTAssertEqual(try fixture.store.existingSourceIDs(), Set(files.map(\.id)))
        await fixture.engine.disconnect()
    }

    // MARK: - 3. 自动同步关闭：只拉列表，手动勾选下载

    func testAutoSyncOffManualSelection() async throws {
        let files = sampleFiles()
        var config = SyncConfig.testDefault
        config.autoSyncEnabled = false
        let fixture = makeSyncFixture(files: files, config: config)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        // 事件收集：手动下载完成必须发 syncCompleted（app 层入库即转依赖该事件，
        // 见 CI run 39040395432 复盘——库页不在层级时靠 VM 订阅此信号派发补转）
        let events = SyncEventCollector()
        let collectingEvents = collectEvents(of: fixture.engine, into: events)
        defer { collectingEvents.cancel() }

        await fixture.engine.connect()
        let listed = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready
                && snapshot.rows.count == 3
                && snapshot.rows.allSatisfy { $0.status == .notDownloaded }
        }
        XCTAssertTrue(listed, "autoSync 关闭时只拉列表不入队")
        XCTAssertEqual(try fixture.store.existingSourceIDs(), [], "不应自动下载")

        await fixture.engine.downloadSelected([files[1].id])
        let downloaded = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready
                && snapshot.rows.first { $0.id == files[1].id }?.status == .verified
        }
        XCTAssertTrue(downloaded, "手动勾选应下载所选")
        let eventArrived = await pollUntil(timeout: 2) {
            events.values.contains { $0 == .syncCompleted(newFiles: 1) }
        }
        XCTAssertTrue(eventArrived, "手动下载完成应发 syncCompleted(1)（入库即转的事件源）")
        let snapshot = await fixture.engine.snapshot
        XCTAssertEqual(
            snapshot.rows.filter { $0.status == .verified }.map(\.id),
            [files[1].id],
            "只下载所选"
        )
        XCTAssertEqual(try fixture.store.existingSourceIDs(), [files[1].id])
        await fixture.engine.disconnect()
    }

    // MARK: - 4. 幂等重同步：refresh 不产生重复下载

    func testIdempotentResync() async throws {
        let files = sampleFiles()
        let fixture = makeSyncFixture(files: files)
        defer { try? FileManager.default.removeItem(at: fixture.root) }

        await fixture.engine.connect()
        let synced = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready
                && snapshot.rows.count == 3
                && snapshot.rows.allSatisfy { $0.status == .verified }
        }
        XCTAssertTrue(synced)

        // 刷新：只应有 LIST_REQ 一帧（无 DL_BEGIN 即无重下）
        let countsBefore = fixture.transport.receivedFrameTypeCounts
        await fixture.engine.refresh()
        // 等 refresh 的列表帧真实到达且状态回到 ready（防竞态：ready 可能先于帧到达被读到）
        let settled = await pollUntil(timeout: 5) {
            let counts = fixture.transport.receivedFrameTypeCounts
            let listDelta = (counts[BuzzLinkFrameType.listReq.rawValue] ?? 0)
                - (countsBefore[BuzzLinkFrameType.listReq.rawValue] ?? 0)
            let state = await fixture.engine.snapshot.state
            return listDelta >= 1 && state == .ready
        }
        XCTAssertTrue(settled, "refresh 应重新拉一次列表并回 ready")
        let countsAfter = fixture.transport.receivedFrameTypeCounts
        XCTAssertEqual(
            (countsAfter[BuzzLinkFrameType.dlBeginReq.rawValue] ?? 0)
                - (countsBefore[BuzzLinkFrameType.dlBeginReq.rawValue] ?? 0),
            0, "幂等重同步不应发起 DL_BEGIN"
        )

        // 行态与库内容不变
        let snapshot = await fixture.engine.snapshot
        XCTAssertTrue(snapshot.rows.allSatisfy { $0.status == .verified })
        XCTAssertEqual(try fixture.store.existingSourceIDs(), Set(files.map(\.id)))
        let dirs = try SyncTestSupport.libraryDirectoryNames(root: fixture.root)
        XCTAssertEqual(dirs.count, 3, "不产生副本目录")
        await fixture.engine.disconnect()
    }
}
