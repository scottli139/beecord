import BuzzLink
import BuzzLinkCore
import BuzzLinkSimCore
import BuzzLinkSync
import BuzzLinkTestSupport
import XCTest

/// 进度聚合与边界行为测试：字节加权整体进度/速率/ETA、空列表、存储告警、
/// 异常文件名入库安全化、空闲保活。
final class SyncEngineProgressTests: XCTestCase {

    // MARK: - 1. 进度聚合：字节加权 overall、滑动窗口速率、ETA、单调性

    func testProgressAggregation() async throws {
        let files = [
            SyncTestSupport.makeFile(id: "REC_20260101_120000", size: 50 * 224, seed: 31),
            SyncTestSupport.makeFile(id: "REC_20260102_093000", size: 50 * 224, seed: 32)
        ]
        // 限速 ~0.7s/文件，产生足够进度采样点；看门狗放大：慢 runner 调度停顿会误触发整文件重试
        let fixture = makeSyncFixture(
            files: files,
            throttleBytesPerSecond: 16_000,
            commandTimeout: 3,
            chunkTimeout: 5
        )
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let snapshots = SnapshotCollector()
        let collecting = collectSnapshots(of: fixture.engine, into: snapshots)
        defer { collecting.cancel() }

        await fixture.engine.connect()
        // 名义 ~1.4s（16KB/s × 2 文件）；慢 runner 拉伸下 10s 余量不足（同套件修复记录），放宽到 30s
        let done = await pollUntil(timeout: 30) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready
                && snapshot.rows.count == 2
                && snapshot.rows.allSatisfy { $0.status == .verified }
        }
        XCTAssertTrue(done)

        // 快照流经消费任务异步投递：actor 侧 ready 不保证记录器已 append 末帧
        // （终态快照 = state .ready + 满格进度）；有界轮询等收敛再读，防 CI 慢 runner 抢跑
        let drained = await pollUntil(timeout: 5) {
            snapshots.values.last?.state == .ready
        }
        XCTAssertTrue(drained, "快照记录应收敛到终态")
        let progresses = snapshots.values.compactMap(\.progress)
        XCTAssertFalse(progresses.isEmpty, "同步期应有进度快照")
        // 速率与 ETA 曾有效
        XCTAssertTrue(
            progresses.contains { $0.bytesPerSecond > 0 && $0.etaSeconds != nil },
            "有数据流动时速率应为正且 ETA 非空"
        )
        // 全部进度分量在 [0,1]
        XCTAssertTrue(progresses.allSatisfy {
            (0...1).contains($0.fileFraction) && (0...1).contains($0.overallFraction)
        })
        // 队列尺寸正确
        XCTAssertTrue(progresses.allSatisfy { $0.totalFiles == 2 })
        XCTAssertTrue(progresses.contains { $0.completedFiles == 0 })
        XCTAssertTrue(progresses.contains { $0.completedFiles >= 1 }, "首文件完成后 completedFiles 应推进")
        // 字节加权整体进度单调不降（本用例无故障=无重试趟回落），且终态满格
        let overalls = progresses.map(\.overallFraction)
        XCTAssertEqual(overalls, overalls.sorted(), "整体进度应单调不降")
        XCTAssertEqual(overalls.last, 1.0, "队列结束应发满格进度")
        await fixture.engine.disconnect()
    }

    // MARK: - 2. 空文件列表：空态，无下载无事件

    func testEmptyFileList() async throws {
        var faults = SimFaults()
        faults.emptyFileList = true
        let fixture = makeSyncFixture(files: [SyncTestSupport.makeFile(id: "REC_20260101_120000", size: 224, seed: 41)], faults: faults)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let events = SyncEventCollector()
        let collecting = collectEvents(of: fixture.engine, into: events)
        defer { collecting.cancel() }

        await fixture.engine.connect()
        let settled = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready && snapshot.rows.isEmpty
        }
        XCTAssertTrue(settled, "空列表应直接回 ready 空态")
        XCTAssertEqual(try fixture.store.existingSourceIDs(), [])
        XCTAssertTrue(events.values.isEmpty, "无新文件不应发完成事件")
        await fixture.engine.disconnect()
    }

    // MARK: - 3. 存储将满（>90% 占用）：快照告警标志

    func testStorageNearlyFullWarning() async throws {
        var faults = SimFaults()
        faults.storageNearlyFull = true
        let fixture = makeSyncFixture(
            files: [SyncTestSupport.makeFile(id: "REC_20260101_120000", size: 224, seed: 42)],
            faults: faults
        )
        defer { try? FileManager.default.removeItem(at: fixture.root) }

        await fixture.engine.connect()
        let warned = await pollUntil(timeout: 5) {
            await fixture.engine.snapshot.isStorageNearlyFull
        }
        XCTAssertTrue(warned, "剩余 5%（>90% 占用）应触发告警标志")
        // 告警不阻塞同步
        let done = await pollUntil(timeout: 5) {
            await fixture.engine.snapshot.rows.first?.status == .verified
        }
        XCTAssertTrue(done)
        await fixture.engine.disconnect()
    }

    // MARK: - 4. 异常文件名：入库目录名安全化 + 幂等

    func testHostileFileNamesSanitized() async throws {
        var faults = SimFaults()
        faults.hostileFileNames = true
        let fixture = makeSyncFixture(
            files: [SyncTestSupport.makeFile(id: "REC_20260101_120000", size: 2 * 224, seed: 43)],
            faults: faults
        )
        defer { try? FileManager.default.removeItem(at: fixture.root) }

        await fixture.engine.connect()
        let done = await pollUntil(timeout: 10) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready
                && snapshot.rows.count == 3
                && snapshot.rows.allSatisfy { $0.status == .verified }
        }
        XCTAssertTrue(done, "正常 + 2 个异常文件名条目应全部入库")

        // 目录名安全：无非法字符、长度受限；原始文件 id 保留在 meta.source.fileID
        let dirs = try SyncTestSupport.libraryDirectoryNames(root: fixture.root)
        XCTAssertEqual(dirs.count, 3)
        let invalid = CharacterSet(charactersIn: "/:*?\"<>|\\")
        for dir in dirs {
            XCTAssertEqual(dir.rangeOfCharacter(from: invalid), nil, "目录名不得含非法字符：\(dir)")
            XCTAssertLessThanOrEqual(dir.count, 48, "目录名应截断至 48 字符：\(dir)")
        }
        let sourceIDs = try fixture.store.existingSourceIDs()
        XCTAssertTrue(sourceIDs.contains("REC_2026/06:06*?"), "幂等键保留原始设备文件 id")
        XCTAssertTrue(sourceIDs.contains("REC_超长文件名截断测ABCD"))

        // 幂等：refresh 后目录不增、内容不重下（等列表帧真实到达，防"ready 抢跑"竞态）
        let countsBefore = fixture.transport.receivedFrameTypeCounts
        await fixture.engine.refresh()
        let settled = await pollUntil(timeout: 5) {
            let listDelta = (fixture.transport.receivedFrameTypeCounts[BuzzLinkFrameType.listReq.rawValue] ?? 0)
                - (countsBefore[BuzzLinkFrameType.listReq.rawValue] ?? 0)
            let state = await fixture.engine.snapshot.state
            return listDelta >= 1 && state == .ready
        }
        XCTAssertTrue(settled)
        let beginDelta = (fixture.transport.receivedFrameTypeCounts[BuzzLinkFrameType.dlBeginReq.rawValue] ?? 0)
            - (countsBefore[BuzzLinkFrameType.dlBeginReq.rawValue] ?? 0)
        XCTAssertEqual(beginDelta, 0, "重复同步不应重下")
        XCTAssertEqual(try SyncTestSupport.libraryDirectoryNames(root: fixture.root).count, 3, "重复同步不产生副本")
        await fixture.engine.disconnect()
    }

    // MARK: - 5. 空闲保活：ready 空闲发 PING；下载期间不发

    func testKeepalivePingWhenIdleOnly() async throws {
        let file = SyncTestSupport.makeFile(id: "REC_20260101_120000", size: 100 * 224, seed: 44)
        var config = SyncConfig.testDefault
        config.autoSyncEnabled = false
        config.keepaliveInterval = 0.1
        // 看门狗放大：慢 runner 上 8KB/s 节流块的 ~30ms 间隔会被调度拉长，0.3s 默认值会误判静默
        let fixture = makeSyncFixture(
            files: [file],
            config: config,
            throttleBytesPerSecond: 8_000,
            commandTimeout: 3,
            chunkTimeout: 5
        )
        defer { try? FileManager.default.removeItem(at: fixture.root) }

        await fixture.engine.connect()
        let ready = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready && snapshot.rows.count == 1
        }
        XCTAssertTrue(ready)

        // 空闲：PING 按 0.1s 间隔发出
        let pingType = BuzzLinkFrameType.ping.rawValue
        let baseline = fixture.transport.receivedFrameTypeCounts[pingType] ?? 0
        let pinged = await pollUntil(timeout: 2) {
            (fixture.transport.receivedFrameTypeCounts[pingType] ?? 0) > baseline
        }
        XCTAssertTrue(pinged, "ready 空闲应按 keepaliveInterval 发 PING")

        // 下载期间（~2.8s）：流量即保活（§6.1），不再发 PING
        // （边界容忍 1：转入 syncing 前恰有一拍在飞 PING 属合法时序）
        await fixture.engine.downloadSelected([file.id])
        let pingAtStart = fixture.transport.receivedFrameTypeCounts[pingType] ?? 0
        // 名义 ~2.8s（8KB/s 单文件）；慢 runner 拉伸可数倍于此，轮询上限放宽到 30s
        let downloaded = await pollUntil(timeout: 30) {
            await fixture.engine.snapshot.rows.first?.status == .verified
        }
        XCTAssertTrue(downloaded)
        let pingDelta = (fixture.transport.receivedFrameTypeCounts[pingType] ?? 0) - pingAtStart
        XCTAssertLessThanOrEqual(pingDelta, 1, "下载期间不应周期发 PING，实际多发 \(pingDelta) 个")
        await fixture.engine.disconnect()
    }
}

/// 速率估算器单测（滑动窗口语义）
final class RateEstimatorTests: XCTestCase {

    func testRateEstimatorBasics() {
        var estimator = RateEstimator(windowSeconds: 5)
        XCTAssertEqual(estimator.bytesPerSecond, 0, "无样本为 0")
        estimator.addSample(t: 0, cumulativeBytes: 0)
        XCTAssertEqual(estimator.bytesPerSecond, 0, "单样本为 0")
        estimator.addSample(t: 1, cumulativeBytes: 1_000)
        XCTAssertEqual(estimator.bytesPerSecond, 1_000, "两点斜率")
    }

    func testRateEstimatorWindowEviction() {
        var estimator = RateEstimator(windowSeconds: 5)
        estimator.addSample(t: 0, cumulativeBytes: 0)
        estimator.addSample(t: 1, cumulativeBytes: 100)
        estimator.addSample(t: 6, cumulativeBytes: 600)
        // t=0 样本被逐出窗口：速率 = (600-100)/(6-1) = 100
        XCTAssertEqual(estimator.bytesPerSecond, 100, accuracy: 0.001)
    }

    func testRateEstimatorStallDecaysToZero() {
        var estimator = RateEstimator(windowSeconds: 1)
        estimator.addSample(t: 0, cumulativeBytes: 0)
        estimator.addSample(t: 0.5, cumulativeBytes: 1_000)
        XCTAssertGreaterThan(estimator.bytesPerSecond, 0)
        // 停滞：累计不变推进时间，窗口内样本归零差
        for tick in 1...10 {
            estimator.addSample(t: 0.5 + Double(tick) * 0.2, cumulativeBytes: 1_000)
        }
        XCTAssertEqual(estimator.bytesPerSecond, 0, "停滞后速率应衰减为 0")
    }
}
