import BuzzLink
import BuzzLinkCore
import BuzzLinkSimCore
import BuzzLinkSync
import XCTest

/// 自动对时（BUZZLINK.md v0.2 §6.1 step 3）：INFO→TIME_SET→LIST 顺序、
/// 时区/epoch 正确性、失败降级不阻断（旧设备超时 / 设备拒绝）。
final class SyncEngineTimeSetTests: XCTestCase {

    private func sampleFiles() -> [SimFile] {
        [SyncTestSupport.makeFile(id: "REC_20260101_120000", durationMs: 5_000,
                                  size: 5 * 224, seed: 1)]
    }

    func testAutoTimeSetBetweenInfoAndList() async throws {
        let fixture = makeSyncFixture(files: sampleFiles())
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        // 注入确定性时间源：2024-01-01T00:00:00Z + UTC+8（北京）
                await fixture.engine.setTimeSource { (1_704_067_200_000, 480) }

        let events = SyncEventCollector()
        let collecting = collectEvents(of: fixture.engine, into: events)
        defer { collecting.cancel() }
        await fixture.engine.connect()
        let done = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready && snapshot.rows.count == 1
        }
        XCTAssertTrue(done, "对时后同步应照常完成")
        if fixture.transport.lastAcceptedTimeSet == nil {
            XCTFail("TIME_SET 未被接受；事件流：\(events.values)")
        }
        XCTAssertEqual(fixture.transport.lastAcceptedTimeSet?.epochMs, 1_704_067_200_000)
        XCTAssertEqual(fixture.transport.lastAcceptedTimeSet?.utcOffsetMin, 480)
    }

    func testTimeSetRejectedDoesNotBlockListing() async throws {
        // Sim 校验失败路径：epoch 越界（设备回 4000）——SyncEngine 应发
        // timeSetFailed 事件但列表/下载照常完成
        let fixture = makeSyncFixture(files: sampleFiles())
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        await fixture.engine.setTimeSource { (9_999_999_999_999, 0) } // 越界 → 设备 4000
        let events = SyncEventCollector()
        let collecting = collectEvents(of: fixture.engine, into: events)
        defer { collecting.cancel() }

        await fixture.engine.connect()
        let done = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready && snapshot.rows.count == 1
        }
        XCTAssertTrue(done, "对时失败不得阻断列表与同步")
        XCTAssertTrue(events.values.contains { event in
            if case .timeSetFailed = event { return true }
            return false
        }, "应发 timeSetFailed 可观测事件")
        XCTAssertNil(fixture.transport.lastAcceptedTimeSet, "拒绝的值不得被记录")
    }

    func testLocalLandingOutOfWindowRejected() async throws {
        // BUZZLINK §8.4：epoch=2099-12-31T23:59:59Z 配 +840min → 本地落点 2100-01-01，
        // 年份出 2000..2099 窗口 → 设备 4000、不记录、同步不阻断
        // （epoch 本身在客户端预校验窗口内，能真实到达设备）
        let fixture = makeSyncFixture(files: sampleFiles())
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        await fixture.engine.setTimeSource { (4_102_444_799_000, 840) }
        let events = SyncEventCollector()
        let collecting = collectEvents(of: fixture.engine, into: events)
        defer { collecting.cancel() }

        await fixture.engine.connect()
        let done = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready && snapshot.rows.count == 1
        }
        XCTAssertTrue(done, "本地落点越界不得阻断同步")
        XCTAssertNil(fixture.transport.lastAcceptedTimeSet)
        XCTAssertTrue(events.values.contains { event in
            if case .timeSetFailed = event { return true }
            return false
        })
    }

    func testOldDeviceTimeoutDegradesSilently() async throws {
        // 旧固件（v0.1）忽略未知 type → 客户端 commandTimeout：
        // 仅 warning、无 timeSetFailed 事件、同步照常完成。
        var faults = SimFaults()
        faults.ignoreTimeSet = true
        let fixture = makeSyncFixture(files: sampleFiles(), faults: faults)
        defer { try? FileManager.default.removeItem(at: fixture.root) }
        let events = SyncEventCollector()
        let collecting = collectEvents(of: fixture.engine, into: events)
        defer { collecting.cancel() }

        await fixture.engine.connect()
        let done = await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready && snapshot.rows.count == 1
        }
        XCTAssertTrue(done, "旧设备（对时超时）同步应降级完成")
        XCTAssertNil(fixture.transport.lastAcceptedTimeSet)
        XCTAssertFalse(events.values.contains { event in
            if case .timeSetFailed = event { return true }
            return false
        }, "超时降级不得发 timeSetFailed 事件（仅 warning 日志）")
    }
}
