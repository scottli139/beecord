import XCTest
@testable import BuzzLinkCore

/// 会话测试向量（TESTING.md §3）
final class TestVectorTests: XCTestCase {

    private func loadFixture(_ name: String = "basic-session") throws -> BuzzLinkTestVector {
        let url = try XCTUnwrap(
            Bundle.module.url(forResource: name, withExtension: "json")
        )
        return try JSONDecoder().decode(BuzzLinkTestVector.self, from: Data(contentsOf: url))
    }

    func testFixtureParses() throws {
        let vector = try loadFixture()
        XCTAssertEqual(vector.schema, BuzzLinkTestVector.currentSchema)
        XCTAssertEqual(vector.entries.count, 6)
        let first = vector.entries[0]
        XCTAssertEqual(first.direction, .tx)
        XCTAssertEqual(first.type, BuzzLinkFrameType.ping.rawValue)
        XCTAssertEqual(first.channel, BuzzLinkChannel.link.rawValue)
        XCTAssertEqual(first.session, 0x2A)
        XCTAssertEqual(try first.decodedPayload(), Data())
    }

    /// 会话向量逐条回放：entry → 帧 → 编码 → 字节流解析 → 与 entry 一致
    func testFixtureEntriesRoundTripThroughParser() throws {
        let vector = try loadFixture()
        var parser = BuzzLinkFrameParser()
        var reassembler = BuzzLinkMessageReassembler()
        var decoded: [BuzzLinkFrame] = []
        for entry in vector.entries {
            let frame = try entry.makeFrame(defaultSession: 0)
            parser.append(try frame.encoded())
            while let parsed = parser.nextFrame() {
                if let message = try reassembler.accept(parsed) {
                    decoded.append(message)
                }
            }
        }
        XCTAssertEqual(parser.discardedFrameCount, 0)
        XCTAssertEqual(decoded.count, vector.entries.count)
        for (frame, entry) in zip(decoded, vector.entries) {
            XCTAssertEqual(frame.type, entry.type)
            XCTAssertEqual(frame.channel, entry.channel)
            XCTAssertEqual(frame.session, entry.session)
            XCTAssertEqual(frame.payload, try entry.decodedPayload())
        }
    }

    func testINFOEntryPayloadIsExpectedJSON() throws {
        let vector = try loadFixture()
        let info = vector.entries[3]
        XCTAssertEqual(info.type, BuzzLinkFrameType.info.rawValue)
        let json = try JSONSerialization.jsonObject(with: info.decodedPayload()) as? [String: Any]
        XCTAssertEqual(json?["deviceName"] as? String, "BCD-DEMO")
        XCTAssertEqual(json?["protocol"] as? Int, 1)
    }

    func testHexStringFieldsAccepted() throws {
        // type/channel 接受 "0x30"、"30"、数字三种写法
        let json = """
        {
          "schema": "buzzlink-vector/1",
          "entries": [
            { "direction": "tx", "type": "0x30", "channel": "0x0002", "payloadHex": "" },
            { "direction": "rx", "type": "31", "channel": "0002", "payloadHex": "" },
            { "direction": "tx", "type": 50, "channel": 2, "payloadHex": "" }
          ]
        }
        """
        let vector = try JSONDecoder().decode(BuzzLinkTestVector.self, from: Data(json.utf8))
        XCTAssertEqual(vector.entries[0].type, 0x30)
        XCTAssertEqual(vector.entries[0].channel, 0x0002)
        XCTAssertEqual(vector.entries[1].type, 0x31)
        XCTAssertEqual(vector.entries[2].type, 50)
        XCTAssertNil(vector.entries[0].session)
    }

    func testPayloadHexToleratesWhitespaceAndCase() throws {
        let entry = BuzzLinkTestVector.Entry(
            direction: .rx, type: 0x11, channel: 0x0001,
            payloadHex: "BE EC 01 02"
        )
        XCTAssertEqual(try entry.decodedPayload(), dataFromHex("be ec 01 02"))
    }

    func testOddLengthHexThrows() {
        let entry = BuzzLinkTestVector.Entry(
            direction: .rx, type: 0x11, channel: 0x0001, payloadHex: "abc"
        )
        XCTAssertThrowsError(try entry.decodedPayload()) {
            XCTAssertEqual($0 as? BuzzLinkVectorError, .malformedHex("abc"))
        }
    }

    func testInvalidHexCharacterThrows() {
        let entry = BuzzLinkTestVector.Entry(
            direction: .rx, type: 0x11, channel: 0x0001, payloadHex: "zz"
        )
        XCTAssertThrowsError(try entry.decodedPayload()) {
            XCTAssertEqual($0 as? BuzzLinkVectorError, .malformedHex("zz"))
        }
    }

    func testUnknownSchemaRejected() {
        let json = """
        { "schema": "buzzlink-vector/99", "entries": [] }
        """
        XCTAssertThrowsError(try JSONDecoder().decode(BuzzLinkTestVector.self, from: Data(json.utf8))) {
            XCTAssertEqual($0 as? BuzzLinkVectorError, .unsupportedSchema("buzzlink-vector/99"))
        }
    }

    func testVectorEncodeRoundTrip() throws {
        let vector = try loadFixture()
        let encoded = try JSONEncoder().encode(vector)
        let decoded = try JSONDecoder().decode(BuzzLinkTestVector.self, from: encoded)
        XCTAssertEqual(decoded, vector)
    }
}
