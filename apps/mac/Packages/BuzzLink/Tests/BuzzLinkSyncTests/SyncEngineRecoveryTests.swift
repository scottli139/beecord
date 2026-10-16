import BuzzLink
import BuzzLinkCore
import BuzzLinkSimCore
import BuzzLinkSync
import BuzzLinkTestSupport
import XCTest

/// 同步恢复与异常路径测试（TESTING.md §5 故障注入矩阵的引擎层用例）：
/// .partial 残留清理、丢块重试、CRC 失败行内标记 + 人工重试、断连自动重连重下、取消。
final class SyncEngineRecoveryTests: XCTestCase {

    // MARK: - 1. 启动清理：.partial 存在即未完成

    func testPartialCleanupOnConnect() async throws {
        let file = SyncTestSupport.makeFile(id: "REC_20260101_120000", size: 5 * 224, seed: 11)
        let fixture = makeSyncFixture(files: [file])
        defer { try? FileManager.default.removeItem(at: fixture.root) }

        // 造残留：上次会话未完成下载的临时文件
        let stale = try fixture.store.preparePartial(for: file.id)
        try Data(repeating: 0x1, count: 100).write(to: stale)
        XCTAssertEqual(SyncTestSupport.partialFileNames(root: fixture.root).count, 1)

        // 连接（connect 先清理残留）→ 自动同步重下该文件
        await fixture.engine.connect()
        let done = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready
                && snapshot.rows.first?.status == .verified
        }
        XCTAssertTrue(done, "残留不影响重下")
        XCTAssertTrue(SyncTestSupport.partialFileNames(root: fixture.root).isEmpty, ".partial 应无残留")
        let audio = try Data(contentsOf: fixture.store.libraryURL
            .appendingPathComponent(file.id).appendingPathComponent("audio.ogg"))
        XCTAssertEqual(audio, file.data)
        await fixture.engine.disconnect()
    }

    // MARK: - 2. 随机丢块：客户端整文件重试后成功（§7）

    func testDropChunksRetrySucceeds() async throws {
        let fileID = "REC_20260101_120000"
        let file = SyncTestSupport.makeFile(id: fileID, size: 10 * 224, seed: 5)
        var faults = SimFaults()
        faults.dropChunkProbability = 0.25
        // 确定性种子：第一趟丢块、第二趟干净（引擎 RNG 跨趟连续，同 T2 手法）
        let seed = try findSeedFirstDropsSecondClean(file: file, faults: faults)
        let fixture = makeSyncFixture(files: [file], faults: faults, randomSeed: seed)
        defer { try? FileManager.default.removeItem(at: fixture.root) }

        await fixture.engine.connect()
        let done = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready && snapshot.rows.first?.status == .verified
        }
        XCTAssertTrue(done, "丢块应经整文件重试最终成功")
        let audio = try Data(contentsOf: fixture.store.libraryURL
            .appendingPathComponent(fileID).appendingPathComponent("audio.ogg"))
        XCTAssertEqual(audio, file.data)
        await fixture.engine.disconnect()
    }

    // MARK: - 3. 末帧 CRC 持续错误：行内失败标记 + 人工重试恢复

    func testCorruptCRCMarksFailedAndManualRetry() async throws {
        let fileID = "REC_20260101_120000"
        let file = SyncTestSupport.makeFile(id: fileID, size: 772, seed: 8)
        var faults = SimFaults()
        faults.corruptFinalChunkCRC = true
        let fixture = makeSyncFixture(files: [file], faults: faults)
        defer { try? FileManager.default.removeItem(at: fixture.root) }

        await fixture.engine.connect()
        let failed = await pollUntil(timeout: 10) {
            let snapshot = await fixture.engine.snapshot
            guard let row = snapshot.rows.first else { return false }
            if case .failed = row.status { return snapshot.state == .ready }
            return false
        }
        XCTAssertTrue(failed, "CRC 持续失败应标行内失败（单文件连败 1 < 3，全局回 ready）")
        XCTAssertEqual(try fixture.store.existingSourceIDs(), [], "失败文件不入库")
        XCTAssertTrue(SyncTestSupport.partialFileNames(root: fixture.root).isEmpty, "失败后 .partial 应清理")

        // 故障恢复后行内重试 → 成功
        fixture.transport.updateFaults { $0.corruptFinalChunkCRC = false }
        await fixture.engine.retry(fileID)
        let retried = await pollUntil(timeout: 5) {
            await fixture.engine.snapshot.rows.first?.status == .verified
        }
        XCTAssertTrue(retried, "人工重试应成功")
        let audio = try Data(contentsOf: fixture.store.libraryURL
            .appendingPathComponent(fileID).appendingPathComponent("audio.ogg"))
        XCTAssertEqual(audio, file.data)
        await fixture.engine.disconnect()
    }

    // MARK: - 4. 推流中途断连：自动重连 + 重下（TESTING.md §5 真断连用直接断传输）

    func testDisconnectMidDownloadReconnectsAndResumes() async throws {
        let file = SyncTestSupport.makeFile(id: "REC_20260101_120000", size: 100 * 224, seed: 9)
        // 限速 ~2.8s 下完，留出断连窗口；看门狗放大：慢 runner 上块间隔会被调度拉长，
        // 0.3s 的默认 chunkTimeout 会把调度停顿误判为链路静默，触发无谓的整文件重试
        let fixture = makeSyncFixture(
            files: [file],
            throttleBytesPerSecond: 8_000,
            commandTimeout: 3,
            chunkTimeout: 5
        )
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let snapshots = SnapshotCollector()
        let events = SyncEventCollector()
        let collectingSnapshots = collectSnapshots(of: fixture.engine, into: snapshots)
        let collectingEvents = collectEvents(of: fixture.engine, into: events)
        defer { collectingSnapshots.cancel(); collectingEvents.cancel() }

        await fixture.engine.connect()
        let downloading = await pollUntil(timeout: 5) {
            await fixture.engine.snapshot.rows.first?.status.isDownloading == true
        }
        XCTAssertTrue(downloading, "应进入下载中")

        // 真断连（等价退出 Sim app 路径）
        fixture.transport.disconnect()

        // 重下单文件名义 ~2.8s；慢 runner/高负载下节流拉伸可数倍于此（本机 7 路 CPU 抢占
        // 实测 >3.5x），轮询上限放宽到 30s（有界轮询，非固定 sleep）
        let reconnected = await pollUntil(timeout: 30) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready && snapshot.rows.first?.status == .verified
        }
        XCTAssertTrue(reconnected, "断连后应自动重连并重下完成")
        XCTAssertTrue(snapshots.states.contains(.reconnecting), "应出现过 reconnecting 态")
        XCTAssertTrue(events.values.contains(.disconnected), "应发断连事件")
        let audio = try Data(contentsOf: fixture.store.libraryURL
            .appendingPathComponent(file.id).appendingPathComponent("audio.ogg"))
        XCTAssertEqual(audio, file.data)
        XCTAssertTrue(SyncTestSupport.partialFileNames(root: fixture.root).isEmpty)
        await fixture.engine.disconnect()
    }

    // MARK: - 5. 取消同步：队列停止、行回未下载、无残留、可再下

    func testCancelSyncStopsQueue() async throws {
        let files = [
            SyncTestSupport.makeFile(id: "REC_20260101_120000", size: 100 * 224, seed: 21),
            SyncTestSupport.makeFile(id: "REC_20260102_093000", size: 100 * 224, seed: 22)
        ]
        // 限速 ~2.8s/文件，取消发生在第一文件下载中；看门狗放大同用例 4：
        // 慢 runner 调度停顿 >0.3s 会误触发整文件重试，把 ~6s 的重下拖过轮询窗口
        let fixture = makeSyncFixture(
            files: files,
            throttleBytesPerSecond: 8_000,
            commandTimeout: 3,
            chunkTimeout: 5
        )
        defer { try? FileManager.default.removeItem(at: fixture.root) }

        await fixture.engine.connect()
        let downloading = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .syncing && snapshot.rows.contains { $0.status.isDownloading }
        }
        XCTAssertTrue(downloading)

        let cancelAt = ProcessInfo.processInfo.systemUptime
        await fixture.engine.cancelSync()
        let elapsed = ProcessInfo.processInfo.systemUptime - cancelAt
        XCTAssertLessThan(elapsed, 2.0, "合作式取消应快速收敛（配置上限 2s），实际 \(elapsed)s")

        let snapshot = await fixture.engine.snapshot
        XCTAssertEqual(snapshot.state, .ready, "取消后回 ready")
        XCTAssertTrue(snapshot.rows.allSatisfy { $0.status == .notDownloaded }, "下载中行回未下载")
        XCTAssertEqual(try fixture.store.existingSourceIDs(), [], "未提交任何文件")
        XCTAssertTrue(SyncTestSupport.partialFileNames(root: fixture.root).isEmpty, ".partial 应无残留")

        // 取消不毒化后续：手动下载两个文件成功
        await fixture.engine.downloadSelected(files.map(\.id))
        // 名义 ~5.9s（2 × 100 块 × ~30ms）；慢 runner 上 Thread.sleep 节流只保下限，
        // 调度拉伸 + 慢机编译器/测试进程抢占可使实际耗时数倍于此（CI 实测 10s 窗口被击穿），
        // 轮询上限放宽到 30s 保有语义强度（仍为有界轮询，非固定 sleep）
        let redownloaded = await pollUntil(timeout: 30) {
            await fixture.engine.snapshot.rows.allSatisfy { $0.status == .verified }
        }
        XCTAssertTrue(redownloaded, "取消后应可重新下载")
        for file in files {
            let audio = try Data(contentsOf: fixture.store.libraryURL
                .appendingPathComponent(file.id).appendingPathComponent("audio.ogg"))
            XCTAssertEqual(audio, file.data)
        }
        await fixture.engine.disconnect()
    }

    // MARK: - 种子搜索辅助（同 T2：找"第一趟丢块、第二趟干净"的确定性种子）

    private func findSeedFirstDropsSecondClean(file: SimFile, faults: SimFaults) throws -> UInt64 {
        for seed in 0..<1_000 {
            var probe = SimEngine(files: [file], faults: faults, randomSeed: UInt64(seed))
            _ = probe.handle(beginFrame(id: file.id, session: 1), now: 0)
            while probe.nextChunk(now: 0) != nil {}
            let firstDrops = probe.droppedChunkCount
            probe.reset()
            _ = probe.handle(beginFrame(id: file.id, session: 2), now: 0)
            while probe.nextChunk(now: 0) != nil {}
            let secondDrops = probe.droppedChunkCount - firstDrops
            if firstDrops > 0, secondDrops == 0 {
                return UInt64(seed)
            }
        }
        throw XCTSkip("未找到满足条件的随机种子（概率极小，可调高丢块率）")
    }

    /// DL_BEGIN_REQ（§5.1：`[idLen u8][id…]`）
    private func beginFrame(id: String, session: UInt8) -> BuzzLinkFrame {
        var payload = Data([UInt8(id.utf8.count)])
        payload.append(Data(id.utf8))
        return BuzzLinkFrame(
            type: BuzzLinkFrameType.dlBeginReq.rawValue,
            session: session,
            channel: BuzzLinkChannel.file.rawValue,
            payload: payload
        )
    }
}
