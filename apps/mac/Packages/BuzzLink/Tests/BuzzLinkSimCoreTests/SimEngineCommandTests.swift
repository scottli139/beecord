import BuzzLinkCore
import BuzzLinkSimCore
import XCTest

/// 控制通道与链路通道命令行为（BUZZLINK.md §5/§8）。
final class SimEngineCommandTests: XCTestCase {

    private func makeEngine(
        faults: SimFaults = SimFaults(),
        files: [SimFile]? = nil
    ) -> SimEngine {
        SimEngine(
            files: files ?? [
                SimTestSupport.makeFile(id: "REC_20260103_120000", durationMs: 5_000, size: 5_000, seed: 3),
                SimTestSupport.makeFile(id: "REC_20260102_120000", durationMs: 3_000, size: 1_000, seed: 2),
                SimTestSupport.makeFile(id: "REC_20260101_120000", durationMs: 1_000, size: 772, seed: 1)
            ],
            faults: faults
        )
    }

    // MARK: - PING / INFO

    func testPingEchoesPayloadAndSession() throws {
        var engine = makeEngine()
        let client = ClientHarness()
        let ping = BuzzLinkFrame(
            type: BuzzLinkFrameType.ping.rawValue,
            session: 0x2A,
            channel: BuzzLinkChannel.link.rawValue,
            payload: Data([0x01, 0x02])
        )
        let messages = try client.receive(engine.handle(ping, now: 0))
        XCTAssertEqual(messages.count, 1)
        XCTAssertEqual(messages[0].type, BuzzLinkFrameType.pong.rawValue)
        XCTAssertEqual(messages[0].session, 0x2A)
        XCTAssertEqual(messages[0].channel, BuzzLinkChannel.link.rawValue)
        XCTAssertEqual(messages[0].payload, Data([0x01, 0x02]))
    }

    func testInfoPushUsesSessionZero() throws {
        var engine = makeEngine()
        let client = ClientHarness()
        let messages = try client.receive(engine.makeInfoPush())
        XCTAssertEqual(messages.count, 1)
        XCTAssertEqual(messages[0].type, BuzzLinkFrameType.info.rawValue)
        XCTAssertEqual(messages[0].session, 0x00)
    }

    func testInfoGetReturnsProfileJSON() throws {
        var engine = makeEngine()
        let client = ClientHarness()
        let messages = try client.receive(engine.handle(
            SimTestSupport.controlRequest(type: .infoGet, session: 0x10, json: "{}"),
            now: 0
        ))
        XCTAssertEqual(messages.count, 1)
        XCTAssertEqual(messages[0].type, BuzzLinkFrameType.info.rawValue)
        XCTAssertEqual(messages[0].session, 0x10)
        let json = try SimTestSupport.decodeJSONObject(messages[0])
        XCTAssertEqual(json["model"] as? String, "beecord-sim")
        XCTAssertEqual(json["fwVersion"] as? String, "0.1.0")
        XCTAssertEqual(json["protocol"] as? Int, Int(BuzzLinkConstants.version))
        XCTAssertEqual(json["deviceName"] as? String, "BCD-DEMO")
        XCTAssertEqual(json["battery"] as? Int, 87)
        XCTAssertEqual(json["charging"] as? Bool, false)
        XCTAssertEqual(json["storageTotal"] as? UInt64, 31_000_000)
        XCTAssertEqual(json["storageFree"] as? UInt64, 12_345_678)
    }

    func testInfoStorageNearlyFullFault() throws {
        var faults = SimFaults()
        faults.storageNearlyFull = true
        var engine = makeEngine(faults: faults)
        let client = ClientHarness()
        let messages = try client.receive(engine.handle(
            SimTestSupport.controlRequest(type: .infoGet, session: 1, json: "{}"),
            now: 0
        ))
        let json = try SimTestSupport.decodeJSONObject(try XCTUnwrap(messages.first))
        // 剩余 5% → 占用 95% > 90% 阈值（TESTING.md §5）
        XCTAssertEqual(json["storageFree"] as? UInt64, 31_000_000 / 20)
        XCTAssertEqual(json["storageTotal"] as? UInt64, 31_000_000)
    }

    // MARK: - LIST

    func testListReturnsFilesNewestFirst() throws {
        var engine = makeEngine()
        let client = ClientHarness()
        let messages = try client.receive(engine.handle(
            SimTestSupport.controlRequest(type: .listReq, session: 0x2B, json: #"{"offset":0}"#),
            now: 0
        ))
        XCTAssertEqual(messages.count, 1)
        XCTAssertEqual(messages[0].type, BuzzLinkFrameType.listResp.rawValue)
        XCTAssertEqual(messages[0].session, 0x2B)
        let json = try SimTestSupport.decodeJSONObject(messages[0])
        XCTAssertEqual(json["status"] as? String, "ok")
        XCTAssertEqual(json["total"] as? Int, 3)
        XCTAssertEqual(json["offset"] as? Int, 0)
        let files = try XCTUnwrap(json["files"] as? [[String: Any]])
        XCTAssertEqual(files.map { $0["id"] as? String }, [
            "REC_20260103_120000", "REC_20260102_120000", "REC_20260101_120000"
        ])
        XCTAssertEqual(files[0]["size"] as? Int, 5_000)
        XCTAssertEqual(files[0]["durationMs"] as? Int, 5_000)
    }

    func testListPagination() throws {
        var engine = makeEngine()
        let client = ClientHarness()

        // limit 超出 16 截断为 16（§5.1）；本例库小，返回全部
        var messages = try client.receive(engine.handle(
            SimTestSupport.controlRequest(type: .listReq, session: 1, json: #"{"offset":1,"limit":1}"#),
            now: 0
        ))
        var json = try SimTestSupport.decodeJSONObject(try XCTUnwrap(messages.first))
        XCTAssertEqual(json["total"] as? Int, 3)
        XCTAssertEqual(json["offset"] as? Int, 1)
        let page = try XCTUnwrap(json["files"] as? [[String: Any]])
        XCTAssertEqual(page.count, 1)
        XCTAssertEqual(page[0]["id"] as? String, "REC_20260102_120000")

        // offset 越界 → LIST_RESP 内错误 JSON（§8.2）
        messages = try client.receive(engine.handle(
            SimTestSupport.controlRequest(type: .listReq, session: 2, json: #"{"offset":99}"#),
            now: 0
        ))
        XCTAssertEqual(messages.first?.type, BuzzLinkFrameType.listResp.rawValue)
        json = try SimTestSupport.decodeJSONObject(try XCTUnwrap(messages.first))
        XCTAssertEqual(json["status"] as? String, "error")
        XCTAssertEqual(json["code"] as? Int, 4_000)

        // 非法 JSON → ERROR 帧（§9：4000 bad_request）
        messages = try client.receive(engine.handle(
            SimTestSupport.controlRequest(type: .listReq, session: 3, json: "not json"),
            now: 0
        ))
        XCTAssertEqual(messages.first?.type, BuzzLinkFrameType.error.rawValue)
        json = try SimTestSupport.decodeJSONObject(try XCTUnwrap(messages.first))
        XCTAssertEqual(json["code"] as? Int, 4_000)
        XCTAssertEqual(json["status"] as? String, "error")
    }

    func testListEmptyFault() throws {
        var faults = SimFaults()
        faults.emptyFileList = true
        var engine = makeEngine(faults: faults)
        let client = ClientHarness()
        let messages = try client.receive(engine.handle(
            SimTestSupport.controlRequest(type: .listReq, session: 1, json: #"{"offset":0}"#),
            now: 0
        ))
        let json = try SimTestSupport.decodeJSONObject(try XCTUnwrap(messages.first))
        XCTAssertEqual(json["status"] as? String, "ok")
        XCTAssertEqual(json["total"] as? Int, 0)
        XCTAssertEqual((json["files"] as? [Any])?.count, 0)

        // 空列表故障下 FILE_INFO / DL_BEGIN 一律 not_found（行为自洽）
        let infoMessages = try client.receive(engine.handle(
            SimTestSupport.controlRequest(type: .fileInfoReq, session: 2, json: #"{"id":"REC_20260103_120000"}"#),
            now: 0
        ))
        let infoJSON = try SimTestSupport.decodeJSONObject(try XCTUnwrap(infoMessages.first))
        XCTAssertEqual(infoJSON["code"] as? Int, 4_004)
    }

    func testListHostileFileNamesFault() throws {
        var faults = SimFaults()
        faults.hostileFileNames = true
        var engine = makeEngine(faults: faults)
        let client = ClientHarness()
        let messages = try client.receive(engine.handle(
            SimTestSupport.controlRequest(type: .listReq, session: 1, json: #"{"offset":0}"#),
            now: 0
        ))
        let json = try SimTestSupport.decodeJSONObject(try XCTUnwrap(messages.first))
        XCTAssertEqual(json["total"] as? Int, 5)
        let files = try XCTUnwrap(json["files"] as? [[String: Any]])
        let ids = files.compactMap { $0["id"] as? String }
        XCTAssertTrue(ids.contains("REC_2026/06:06*?"))
        XCTAssertTrue(ids.contains("REC_超长文件名截断测ABCD"))
        // 全部 id 都在协议上限内（§5.2：≤ 32 字节）
        for id in ids {
            XCTAssertLessThanOrEqual(id.utf8.count, BuzzLinkConstants.maxFileIDLength)
        }
    }

    func testListResponseFragmentsLargeList() throws {
        // 20 个文件的 LIST_RESP 远超单帧 232 字节上限 → 必须分片（§3.3）
        let files = (1...20).map {
            SimTestSupport.makeFile(id: String(format: "REC_202601%02d_120000", $0), size: 1_000 + $0, seed: UInt64($0))
        }
        var engine = makeEngine(files: files)
        let client = ClientHarness()
        let messages = try client.receive(engine.handle(
            SimTestSupport.controlRequest(type: .listReq, session: 0x40, json: #"{"offset":0}"#),
            now: 0
        ))
        // 客户端视角：分片经重组后恰好一条完整消息
        XCTAssertEqual(messages.count, 1)
        let json = try SimTestSupport.decodeJSONObject(messages[0])
        XCTAssertEqual(json["total"] as? Int, 20)
        XCTAssertEqual((json["files"] as? [Any])?.count, 16) // 每页上限 16（§5.2）
        XCTAssertEqual(messages[0].session, 0x40)
    }

    func testListResponseFragmentsOnTheWire() throws {
        // 不经过客户端重组，直接检查引擎输出的分片帧形态
        let files = (1...20).map {
            SimTestSupport.makeFile(id: String(format: "REC_202601%02d_120000", $0), size: 500, seed: UInt64($0))
        }
        var engine = makeEngine(files: files)
        let outbound = engine.handle(
            SimTestSupport.controlRequest(type: .listReq, session: 0x40, json: #"{"offset":0}"#),
            now: 0
        )
        XCTAssertGreaterThan(outbound.count, 1)
        for (index, item) in outbound.enumerated() {
            XCTAssertTrue(item.frame.isFragment)
            XCTAssertEqual(item.frame.type, BuzzLinkFrameType.listResp.rawValue)
            XCTAssertEqual(item.frame.session, 0x40)
            XCTAssertEqual(item.frame.channel, BuzzLinkChannel.control.rawValue)
            XCTAssertEqual(Int(item.frame.payload.readLE16ForTest(at: 0)), index)
            XCTAssertEqual(Int(item.frame.payload.readLE16ForTest(at: 2)), outbound.count)
            XCTAssertFalse(item.corruptCRC)
        }
    }

    // MARK: - FILE_INFO

    func testFileInfoFound() throws {
        var engine = makeEngine()
        let client = ClientHarness()
        let messages = try client.receive(engine.handle(
            SimTestSupport.controlRequest(type: .fileInfoReq, session: 9, json: #"{"id":"REC_20260101_120000"}"#),
            now: 0
        ))
        XCTAssertEqual(messages.first?.type, BuzzLinkFrameType.fileInfoResp.rawValue)
        let json = try SimTestSupport.decodeJSONObject(try XCTUnwrap(messages.first))
        XCTAssertEqual(json["status"] as? String, "ok")
        XCTAssertEqual(json["id"] as? String, "REC_20260101_120000")
        XCTAssertEqual(json["size"] as? Int, 772)
        XCTAssertEqual(json["format"] as? String, "ogg-opus")
        XCTAssertEqual(json["sampleRate"] as? Int, 16_000)
        XCTAssertEqual(json["channels"] as? Int, 1)
        // crc16 为十进制整数且与内容一致
        let file = SimTestSupport.makeFile(id: "REC_20260101_120000", durationMs: 1_000, size: 772, seed: 1)
        XCTAssertEqual(json["crc16"] as? Int, Int(file.crc16))
    }

    func testFileInfoNotFound() throws {
        var engine = makeEngine()
        let client = ClientHarness()
        let messages = try client.receive(engine.handle(
            SimTestSupport.controlRequest(type: .fileInfoReq, session: 9, json: #"{"id":"REC_29990101_000000"}"#),
            now: 0
        ))
        XCTAssertEqual(messages.first?.type, BuzzLinkFrameType.fileInfoResp.rawValue)
        let json = try SimTestSupport.decodeJSONObject(try XCTUnwrap(messages.first))
        XCTAssertEqual(json["status"] as? String, "error")
        XCTAssertEqual(json["code"] as? Int, 4_004)
    }

    // MARK: - 其他命令与异常

    func testDeleteNotSupported() throws {
        var engine = makeEngine()
        let client = ClientHarness()
        let messages = try client.receive(engine.handle(
            SimTestSupport.controlRequest(type: .fileDeleteReq, session: 5, json: #"{"id":"REC_20260101_120000"}"#),
            now: 0
        ))
        XCTAssertEqual(messages.first?.type, BuzzLinkFrameType.error.rawValue)
        let json = try SimTestSupport.decodeJSONObject(try XCTUnwrap(messages.first))
        XCTAssertEqual(json["code"] as? Int, 4_003)
    }

    func testUnknownFrameTypeGetsErrorFrame() throws {
        var engine = makeEngine()
        let client = ClientHarness()
        let weird = BuzzLinkFrame(type: 0x55, session: 6, channel: BuzzLinkChannel.control.rawValue)
        let messages = try client.receive(engine.handle(weird, now: 0))
        XCTAssertEqual(messages.first?.type, BuzzLinkFrameType.error.rawValue)
        let json = try SimTestSupport.decodeJSONObject(try XCTUnwrap(messages.first))
        XCTAssertEqual(json["code"] as? Int, 4_000)
        XCTAssertEqual(messages.first?.session, 6)
    }

    func testResponseOnlyTypesGetErrorFrame() throws {
        var engine = makeEngine()
        let client = ClientHarness()
        // PONG 是设备→客户端方向帧，客户端不应发来（§5.1）
        let pong = BuzzLinkFrame(type: BuzzLinkFrameType.pong.rawValue, session: 7, channel: BuzzLinkChannel.link.rawValue)
        let messages = try client.receive(engine.handle(pong, now: 0))
        XCTAssertEqual(messages.first?.type, BuzzLinkFrameType.error.rawValue)
    }

    func testFrameCounters() throws {
        var engine = makeEngine()
        // INFO 响应恒为单帧（约 140 字节 < 232 上限），计数确定
        _ = engine.handle(SimTestSupport.controlRequest(type: .infoGet, session: 1, json: "{}"), now: 0)
        _ = engine.handle(SimTestSupport.controlRequest(type: .infoGet, session: 2, json: "{}"), now: 0)
        XCTAssertEqual(engine.receivedFrameCount, 2)
        XCTAssertEqual(engine.sentFrameCount, 2)
    }
}

private extension Data {
    /// 测试内读取小端 UInt16（与生产代码的私有 helper 互不依赖）
    func readLE16ForTest(at offset: Int) -> UInt16 {
        UInt16(self[index(startIndex, offsetBy: offset)]) |
            (UInt16(self[index(startIndex, offsetBy: offset + 1)]) << 8)
    }
}
