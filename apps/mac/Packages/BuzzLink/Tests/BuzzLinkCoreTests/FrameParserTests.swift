import XCTest
@testable import BuzzLinkCore

/// 字节流重组器（BUZZLINK.md §3.3/§10）
final class FrameParserTests: XCTestCase {

    private let pingBytes = dataFromHex("be ec 01 01 2a 00 00 00 00 00 86 7d")
    private let pongBytes = dataFromHex("be ec 01 02 2a 00 00 00 00 00 04 a5")

    func testSingleFrameOneShot() {
        var parser = BuzzLinkFrameParser()
        parser.append(pingBytes)
        let frame = parser.nextFrame()
        XCTAssertEqual(frame?.type, BuzzLinkFrameType.ping.rawValue)
        XCTAssertNil(parser.nextFrame())
        XCTAssertEqual(parser.discardedFrameCount, 0)
    }

    func testByteByByteFeed() {
        var parser = BuzzLinkFrameParser()
        var frames: [BuzzLinkFrame] = []
        for index in pingBytes.indices {
            parser.append(Data([pingBytes[index]]))
            while let frame = parser.nextFrame() {
                frames.append(frame)
            }
        }
        XCTAssertEqual(frames.count, 1)
        XCTAssertEqual(frames.first?.type, BuzzLinkFrameType.ping.rawValue)
    }

    func testTwoFramesInOneAppend() {
        var parser = BuzzLinkFrameParser()
        parser.append(pingBytes + pongBytes)
        XCTAssertEqual(parser.nextFrame()?.type, BuzzLinkFrameType.ping.rawValue)
        XCTAssertEqual(parser.nextFrame()?.type, BuzzLinkFrameType.pong.rawValue)
        XCTAssertNil(parser.nextFrame())
    }

    func testLeadingGarbageSkipped() {
        var parser = BuzzLinkFrameParser()
        parser.append(dataFromHex("00 11 22 be") + pingBytes)
        XCTAssertEqual(parser.nextFrame()?.type, BuzzLinkFrameType.ping.rawValue)
        XCTAssertNil(parser.nextFrame())
    }

    func testHalfMagicAtChunkBoundary() {
        // 第一个分片以魔数首字节结尾：不得误丢
        var parser = BuzzLinkFrameParser()
        let split = 3
        parser.append(pingBytes.prefix(split))
        XCTAssertNil(parser.nextFrame())
        parser.append(pingBytes.suffix(pingBytes.count - split))
        XCTAssertEqual(parser.nextFrame()?.type, BuzzLinkFrameType.ping.rawValue)
    }

    func testCorruptCRCFrameDroppedNextFrameSurvives() throws {
        var corrupt = pingBytes
        corrupt[corrupt.count - 1] ^= 0xFF
        var parser = BuzzLinkFrameParser()
        parser.append(corrupt + pongBytes)
        XCTAssertEqual(parser.nextFrame()?.type, BuzzLinkFrameType.pong.rawValue)
        XCTAssertNil(parser.nextFrame())
        XCTAssertEqual(parser.discardedFrameCount, 1)
    }

    func testFakeMagicInsideCorruptFrameRecovers() throws {
        // payload 内藏假 magic（version 字节非法，确保假帧走重同步路径）
        let fake = BuzzLinkFrame(
            type: BuzzLinkFrameType.info.rawValue,
            session: 1,
            channel: BuzzLinkChannel.control.rawValue,
            payload: dataFromHex("be ec 99 02 03")
        )
        var corrupt = try fake.encoded()
        corrupt[corrupt.count - 1] ^= 0xFF
        var parser = BuzzLinkFrameParser()
        parser.append(corrupt + pongBytes)
        XCTAssertEqual(parser.nextFrame()?.type, BuzzLinkFrameType.pong.rawValue)
        XCTAssertNil(parser.nextFrame())
        XCTAssertGreaterThanOrEqual(parser.discardedFrameCount, 1)
    }

    func testUnknownVersionDroppedAndResynced() {
        // 手工拼 version=0x02 的帧（CRC 合法，但 version 不支持）
        var raw = dataFromHex("be ec 02 01 01 00 00 00 00 00")
        raw.appendLE16(CRC16.ccittFalse(raw.suffix(from: 2)))
        var parser = BuzzLinkFrameParser()
        parser.append(raw)
        XCTAssertNil(parser.nextFrame())
        parser.append(pingBytes)
        XCTAssertEqual(parser.nextFrame()?.type, BuzzLinkFrameType.ping.rawValue)
        XCTAssertGreaterThanOrEqual(parser.discardedFrameCount, 1)
    }

    func testUnknownTypeDroppedButSyncKept() throws {
        let unknown = BuzzLinkFrame(
            type: 0x99, session: 1,
            channel: BuzzLinkChannel.control.rawValue,
            payload: Data([1, 2, 3])
        )
        var parser = BuzzLinkFrameParser()
        parser.append(try unknown.encoded() + pongBytes)
        // 未知 type 帧被忽略，但帧边界可信，后续帧正常解析
        XCTAssertEqual(parser.nextFrame()?.type, BuzzLinkFrameType.pong.rawValue)
        XCTAssertNil(parser.nextFrame())
        XCTAssertEqual(parser.discardedFrameCount, 1)
    }

    func testUnknownFlagsDroppedButSyncKept() throws {
        let unknown = BuzzLinkFrame(
            type: BuzzLinkFrameType.ping.rawValue, session: 1, flags: 0x02,
            channel: BuzzLinkChannel.link.rawValue
        )
        var parser = BuzzLinkFrameParser()
        parser.append(try unknown.encoded() + pongBytes)
        XCTAssertEqual(parser.nextFrame()?.type, BuzzLinkFrameType.pong.rawValue)
        XCTAssertNil(parser.nextFrame())
        XCTAssertEqual(parser.discardedFrameCount, 1)
    }

    func testResetClearsPartialBuffer() {
        var parser = BuzzLinkFrameParser()
        parser.append(pingBytes.prefix(5))
        parser.reset()
        parser.append(pingBytes)
        XCTAssertEqual(parser.nextFrame()?.type, BuzzLinkFrameType.ping.rawValue)
    }
}
