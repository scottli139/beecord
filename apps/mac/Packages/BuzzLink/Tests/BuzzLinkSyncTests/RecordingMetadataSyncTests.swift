import BuzzLink
import BuzzLinkSimCore
import BuzzLinkSync
import XCTest

final class RecordingMetadataSyncTests: XCTestCase {
    private let recordingID1 = String(repeating: "a", count: 32)
    private let recordingID2 = String(repeating: "b", count: 32)

    private func waitForSync(_ fixture: SyncFixture) async -> Bool {
        await fixture.engine.connect()
        return await pollUntil(timeout: 5) {
            let snapshot = await fixture.engine.snapshot
            return snapshot.state == .ready && snapshot.rows.allSatisfy { $0.status == .verified }
        }
    }

    func testSameFileNameAcrossCardsUsesRecordingIDForDedup() async throws {
        let root = SyncTestSupport.makeTempRoot()
        defer { try? FileManager.default.removeItem(at: root) }
        let first = SyncTestSupport.makeFile(id: "REC_0001.OGG", durationMs: 1_000, size: 500, seed: 1)
        var firstModern = first
        firstModern.recordingID = recordingID1
        let fixture1 = makeSyncFixture(files: [firstModern], rootURL: root)
        let done1 = await waitForSync(fixture1)
        XCTAssertTrue(done1)
        await fixture1.engine.disconnect()

        var secondModern = SyncTestSupport.makeFile(
            id: "REC_0001.OGG", durationMs: 2_000, size: 700, seed: 2
        )
        secondModern.recordingID = recordingID2
        let profile2 = SimDeviceProfile(deviceName: "BCD-CARD2", deviceID: "00000000CA22")
        let fixture2 = makeSyncFixture(files: [secondModern], profile: profile2, rootURL: root)
        let done2 = await waitForSync(fixture2)
        XCTAssertTrue(done2)
        await fixture2.engine.disconnect()

        let metas = try fixture2.store.listRecordings()
        XCTAssertEqual(metas.count, 2, "换卡后同名文件不得被去重覆盖")
        XCTAssertEqual(Set(metas.compactMap(\.source.recordingID)), Set([recordingID1, recordingID2]))
        XCTAssertEqual(Set(metas.map(\.source.fileID)), ["REC_0001.OGG"])
        let directories = try SyncTestSupport.libraryDirectoryNames(root: root)
        XCTAssertEqual(Set(directories), Set([recordingID1, recordingID2]))
    }

    func testLegacySameFileNameAcrossDevicesUsesDeviceID() async throws {
        let root = SyncTestSupport.makeTempRoot()
        defer { try? FileManager.default.removeItem(at: root) }
        let profile1 = SimDeviceProfile(deviceName: "BCD-ONE", deviceID: "000000000001")
        let file1 = SyncTestSupport.makeFile(id: "REC_0001.OGG", size: 400, seed: 3)
        let fixture1 = makeSyncFixture(files: [file1], profile: profile1, rootURL: root)
        let done1 = await waitForSync(fixture1)
        XCTAssertTrue(done1)
        await fixture1.engine.disconnect()

        let profile2 = SimDeviceProfile(deviceName: "BCD-TWO", deviceID: "000000000002")
        let file2 = SyncTestSupport.makeFile(id: "REC_0001.OGG", size: 600, seed: 4)
        let fixture2 = makeSyncFixture(files: [file2], profile: profile2, rootURL: root)
        let done2 = await waitForSync(fixture2)
        XCTAssertTrue(done2)
        await fixture2.engine.disconnect()

        let metas = try fixture2.store.listRecordings()
        XCTAssertEqual(metas.count, 2)
        XCTAssertEqual(Set(metas.compactMap(\.source.deviceID)), Set(["000000000001", "000000000002"]))
    }

    func testExplicitTimeAndOriginalOffsetArePersisted() async throws {
        let root = SyncTestSupport.makeTempRoot()
        defer { try? FileManager.default.removeItem(at: root) }
        var modern = SyncTestSupport.makeFile(
            id: "REC_20261122_191221_0001.OGG", durationMs: 5_000, size: 500, seed: 5
        )
        modern.recordingID = recordingID1
        modern.startedAtEpochMs = 1_795_345_941_000
        modern.startedAtUtcOffsetMin = 480
        modern.timeQuality = "synced"
        let legacy = SyncTestSupport.makeFile(
            id: "REC_20260101_120000.OGG", durationMs: 1_000, size: 300, seed: 6
        )
        let fixture = makeSyncFixture(files: [modern, legacy], rootURL: root)
        let done = await waitForSync(fixture)
        XCTAssertTrue(done)
        await fixture.engine.disconnect()

        let metas = try fixture.store.listRecordings()
        guard let modernMeta = metas.first(where: { $0.source.recordingID == recordingID1 }) else {
            return XCTFail("缺少新格式录音")
        }
        XCTAssertEqual(modernMeta.schemaVersion, 2)
        XCTAssertEqual(modernMeta.recordedAt, Date(timeIntervalSince1970: 1_795_345_941))
        XCTAssertEqual(modernMeta.recordedAtUtcOffsetMin, 480)
        XCTAssertEqual(modernMeta.recordedAtSource, .device)
        XCTAssertEqual(modernMeta.recordingTimeQuality, .synced)
        XCTAssertEqual(modernMeta.title, "2026-11-22 19:12 录音")
        XCTAssertEqual(modernMeta.source.deviceID, "00000000BEEF")

        guard let legacyMeta = metas.first(where: { $0.source.fileID == "REC_20260101_120000.OGG" }) else {
            return XCTFail("缺少旧格式录音")
        }
        XCTAssertNotNil(legacyMeta.recordedAt)
        XCTAssertNil(legacyMeta.recordedAtUtcOffsetMin)
        XCTAssertEqual(legacyMeta.recordedAtSource, .filename)
        XCTAssertNil(legacyMeta.source.recordingID)
    }

    func testManualRecordedAtPreservesTitleAndLogicalSource() {
        var meta = SyncTestSupport.makeMeta(
            fileID: "REC_0001.OGG", size: 100, crc16: 123, durationMs: 1_000
        )
        meta.title = "用户标题"
        meta.schemaVersion = 1
        let keys = meta.sourceKeys
        let date = Date(timeIntervalSince1970: 1_795_345_941)
        meta.applyManualRecordedAt(date, utcOffsetMin: 480)
        XCTAssertEqual(meta.title, "用户标题")
        XCTAssertEqual(meta.schemaVersion, 2)
        XCTAssertEqual(meta.sourceKeys, keys)
        XCTAssertEqual(meta.recordedAt, date)
        XCTAssertEqual(meta.recordedAtUtcOffsetMin, 480)
        XCTAssertEqual(meta.recordedAtSource, .manual)
        XCTAssertNil(meta.recordingTimeQuality)
    }
}
