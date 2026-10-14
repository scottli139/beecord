import XCTest
@testable import BuzzLinkCore

/// 帧编解码：以 BUZZLINK.md §11 四条向量为断言基准
final class FrameCodecTests: XCTestCase {

    // MARK: - §11 测试向量

    func testVectorV1Ping() throws {
        let bytes = dataFromHex("be ec 01 01 2a 00 00 00 00 00 86 7d")
        let frame = try BuzzLinkFrame.decoded(from: bytes)
        XCTAssertEqual(frame.type, BuzzLinkFrameType.ping.rawValue)
        XCTAssertEqual(frame.session, 0x2A)
        XCTAssertEqual(frame.channel, BuzzLinkChannel.link.rawValue)
        XCTAssertTrue(frame.payload.isEmpty)
        XCTAssertFalse(frame.isFragment)
        // 编码往返必须与向量逐字节一致
        XCTAssertEqual(try frame.encoded(), bytes)
    }

    func testVectorV2ListReq() throws {
        let bytes = dataFromHex("be ec 01 10 2b 00 01 00 0c 00 7b 22 6f 66 66 73 65 74 22 3a 30 7d df cd")
        let frame = try BuzzLinkFrame.decoded(from: bytes)
        XCTAssertEqual(frame.type, BuzzLinkFrameType.listReq.rawValue)
        XCTAssertEqual(frame.session, 0x2B)
        XCTAssertEqual(frame.channel, BuzzLinkChannel.control.rawValue)
        XCTAssertEqual(String(decoding: frame.payload, as: UTF8.self), #"{"offset":0}"#)
        XCTAssertEqual(try frame.encoded(), bytes)
    }

    func testVectorV3DownloadBeginReq() throws {
        let bytes = dataFromHex("be ec 01 30 31 00 02 00 14 00 13 52 45 43 5f 32 30 32 36 31 30 31 34 5f 30 39 31 35 33 33 d5 0c")
        let frame = try BuzzLinkFrame.decoded(from: bytes)
        XCTAssertEqual(frame.type, BuzzLinkFrameType.dlBeginReq.rawValue)
        XCTAssertEqual(frame.session, 0x31)
        XCTAssertEqual(frame.channel, BuzzLinkChannel.file.rawValue)
        // payload = [idLen u8][id…]
        XCTAssertEqual(Int(frame.payload[frame.payload.startIndex]), 0x13)
        let id = String(decoding: frame.payload.dropFirst(), as: UTF8.self)
        XCTAssertEqual(id, "REC_20261014_091533")
        XCTAssertEqual(try frame.encoded(), bytes)
    }

    func testVectorV4DownloadBeginResp() throws {
        let bytes = dataFromHex("be ec 01 31 31 00 02 00 0d 00 00 e0 00 a6 11 00 00 ac 6e 0f 00 3a 6f 02 47")
        let frame = try BuzzLinkFrame.decoded(from: bytes)
        XCTAssertEqual(frame.type, BuzzLinkFrameType.dlBeginResp.rawValue)
        XCTAssertEqual(frame.channel, BuzzLinkChannel.file.rawValue)
        // payload = [status][chunkDataLen u16][totalChunks u32][size u32][fileCrc16 u16]（13B）
        XCTAssertEqual(frame.payload.count, 13)
        let p = frame.payload
        XCTAssertEqual(p[p.startIndex], BuzzLinkStatus.ok.rawValue)
        XCTAssertEqual(p.readLE16(at: p.startIndex + 1), 224)
        XCTAssertEqual(p.readLE32(at: p.startIndex + 3), 4518)
        XCTAssertEqual(p.readLE32(at: p.startIndex + 7), 1011372)
        XCTAssertEqual(p.readLE16(at: p.startIndex + 11), 0x6F3A)
        XCTAssertEqual(try frame.encoded(), bytes)
    }

    // MARK: - 编解码通用行为

    func testEncodeDecodeRoundTrip() throws {
        let original = BuzzLinkFrame(
            type: BuzzLinkFrameType.fileInfoReq.rawValue,
            session: 0x77,
            flags: BuzzLinkConstants.flagFragment,
            channel: BuzzLinkChannel.control.rawValue,
            payload: Data([0x00, 0x01, 0x02, 0xFE, 0xFF])
        )
        let decoded = try BuzzLinkFrame.decoded(from: original.encoded())
        XCTAssertEqual(decoded, original)
    }

    func testDecodeBadMagicThrows() {
        let bytes = dataFromHex("de ad 01 01 2a 00 00 00 00 00 86 7d")
        XCTAssertThrowsError(try BuzzLinkFrame.decoded(from: bytes)) {
            XCTAssertEqual($0 as? BuzzLinkFrameError, .badMagic)
        }
    }

    func testDecodeTruncatedThrows() {
        let bytes = dataFromHex("be ec 01 01 2a")
        XCTAssertThrowsError(try BuzzLinkFrame.decoded(from: bytes)) {
            XCTAssertEqual($0 as? BuzzLinkFrameError, .truncatedFrame)
        }
    }

    func testDecodeLengthMismatchThrows() {
        // 向量 V1 尾加一字节：length 声明 0 与实际不符
        let bytes = dataFromHex("be ec 01 01 2a 00 00 00 00 00 86 7d 00")
        XCTAssertThrowsError(try BuzzLinkFrame.decoded(from: bytes)) {
            XCTAssertEqual($0 as? BuzzLinkFrameError, .lengthMismatch(declared: 0, actual: 1))
        }
    }

    func testDecodeTamperedPayloadFailsCRC() {
        // 向量 V2 翻 1 个 payload 位 → CRC 必失败
        var bytes = dataFromHex("be ec 01 10 2b 00 01 00 0c 00 7b 22 6f 66 66 73 65 74 22 3a 30 7d df cd")
        bytes[12] ^= 0x01
        XCTAssertThrowsError(try BuzzLinkFrame.decoded(from: bytes)) {
            guard case .crcMismatch = $0 as? BuzzLinkFrameError else {
                return XCTFail("非预期错误：\($0)")
            }
        }
    }

    func testEncodeOversizePayloadThrows() {
        let frame = BuzzLinkFrame(
            type: BuzzLinkFrameType.error.rawValue,
            session: 0,
            channel: BuzzLinkChannel.control.rawValue,
            payload: Data(repeating: 0xAA, count: Int(UInt16.max) + 1)
        )
        XCTAssertThrowsError(try frame.encoded()) {
            XCTAssertEqual($0 as? BuzzLinkFrameError, .payloadTooLarge(Int(UInt16.max) + 1))
        }
    }
}
