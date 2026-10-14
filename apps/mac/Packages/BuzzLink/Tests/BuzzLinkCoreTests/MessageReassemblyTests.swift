import XCTest
@testable import BuzzLinkCore

/// 分片重组与发送（BUZZLINK.md §3.3）
final class MessageReassemblyTests: XCTestCase {

    /// 手工构造一个分片帧
    private func fragFrame(index: UInt16, count: UInt16, session: UInt8 = 1, data: Data) -> BuzzLinkFrame {
        var payload = Data()
        payload.appendLE16(index)
        payload.appendLE16(count)
        payload.append(data)
        return BuzzLinkFrame(
            type: BuzzLinkFrameType.listResp.rawValue,
            session: session,
            flags: BuzzLinkConstants.flagFragment,
            channel: BuzzLinkChannel.control.rawValue,
            payload: payload
        )
    }

    // MARK: - 重组

    func testNonFragmentPassesThrough() throws {
        var reassembler = BuzzLinkMessageReassembler()
        let frame = BuzzLinkFrame(
            type: BuzzLinkFrameType.pong.rawValue, session: 1,
            channel: BuzzLinkChannel.link.rawValue
        )
        XCTAssertEqual(try reassembler.accept(frame), frame)
    }

    /// §11 分片示例：500 字节消息分 3 片（228/228/44）
    func testThreeFragmentReassembly() throws {
        let message = Data((0..<500).map { UInt8($0 % 251) })
        let frames = try BuzzLinkFragmenter.fragment(
            type: BuzzLinkFrameType.listResp.rawValue,
            session: 0x2C,
            channel: BuzzLinkChannel.control.rawValue,
            message: message
        )
        XCTAssertEqual(frames.count, 3)
        XCTAssertEqual(frames[0].payload.count, 232)
        XCTAssertEqual(frames[1].payload.count, 232)
        XCTAssertEqual(frames[2].payload.count, 48)
        for (offset, frame) in frames.enumerated() {
            XCTAssertTrue(frame.isFragment)
            XCTAssertEqual(frame.payload.readLE16(at: frame.payload.startIndex), UInt16(offset))
            XCTAssertEqual(frame.payload.readLE16(at: frame.payload.startIndex + 2), 3)
        }

        var reassembler = BuzzLinkMessageReassembler()
        XCTAssertNil(try reassembler.accept(frames[0]))
        XCTAssertNil(try reassembler.accept(frames[1]))
        let complete = try XCTUnwrap(reassembler.accept(frames[2]))
        XCTAssertEqual(complete.payload, message)
        XCTAssertFalse(complete.isFragment)
        XCTAssertEqual(complete.type, BuzzLinkFrameType.listResp.rawValue)
        XCTAssertEqual(complete.session, 0x2C)
        XCTAssertEqual(complete.channel, BuzzLinkChannel.control.rawValue)
    }

    func testOutOfOrderFragmentsReassemble() throws {
        let message = Data((0..<500).map { UInt8($0 % 251) })
        let frames = try BuzzLinkFragmenter.fragment(
            type: 0x11, session: 7, channel: 0x0001, message: message
        )
        var reassembler = BuzzLinkMessageReassembler()
        XCTAssertNil(try reassembler.accept(frames[1]))
        XCTAssertNil(try reassembler.accept(frames[0]))
        let complete = try XCTUnwrap(reassembler.accept(frames[2]))
        XCTAssertEqual(complete.payload, message)
    }

    func testDuplicateFragmentIgnored() throws {
        let message = Data((0..<500).map { UInt8($0 % 251) })
        let frames = try BuzzLinkFragmenter.fragment(
            type: 0x11, session: 7, channel: 0x0001, message: message
        )
        var reassembler = BuzzLinkMessageReassembler()
        XCTAssertNil(try reassembler.accept(frames[0]))
        XCTAssertNil(try reassembler.accept(frames[0]))
        XCTAssertNil(try reassembler.accept(frames[1]))
        let complete = try XCTUnwrap(reassembler.accept(frames[2]))
        XCTAssertEqual(complete.payload, message)
    }

    func testConflictingDuplicateThrows() {
        var reassembler = BuzzLinkMessageReassembler()
        XCTAssertNoThrow(try reassembler.accept(fragFrame(index: 0, count: 2, data: Data([1, 2, 3]))))
        XCTAssertThrowsError(try reassembler.accept(fragFrame(index: 0, count: 2, data: Data([1, 2, 4])))) {
            XCTAssertEqual($0 as? BuzzLinkReassemblyError, .conflictingFragment(index: 0))
        }
    }

    func testInconsistentFragCountThrows() {
        var reassembler = BuzzLinkMessageReassembler()
        XCTAssertNoThrow(try reassembler.accept(fragFrame(index: 0, count: 3, data: Data([1]))))
        XCTAssertThrowsError(try reassembler.accept(fragFrame(index: 1, count: 4, data: Data([2])))) {
            XCTAssertEqual($0 as? BuzzLinkReassemblyError, .inconsistentFragmentCount)
        }
    }

    func testIndexOutOfRangeThrows() {
        var reassembler = BuzzLinkMessageReassembler()
        XCTAssertThrowsError(try reassembler.accept(fragFrame(index: 3, count: 3, data: Data([1])))) {
            XCTAssertEqual($0 as? BuzzLinkReassemblyError, .fragmentIndexOutOfRange(index: 3, count: 3))
        }
    }

    func testZeroFragCountThrows() {
        var reassembler = BuzzLinkMessageReassembler()
        XCTAssertThrowsError(try reassembler.accept(fragFrame(index: 0, count: 0, data: Data([1])))) {
            XCTAssertEqual($0 as? BuzzLinkReassemblyError, .invalidFragmentCount)
        }
    }

    func testShortFragmentHeaderThrows() {
        var reassembler = BuzzLinkMessageReassembler()
        let frame = BuzzLinkFrame(
            type: 0x11, session: 1, flags: BuzzLinkConstants.flagFragment,
            channel: 0x0001, payload: Data([0x00, 0x01, 0x02])
        )
        XCTAssertThrowsError(try reassembler.accept(frame)) {
            XCTAssertEqual($0 as? BuzzLinkReassemblyError, .fragmentHeaderTooShort)
        }
    }

    func testOversizeMessageThrows() {
        var reassembler = BuzzLinkMessageReassembler()
        let chunk = Data(repeating: 0xAB, count: 228)
        XCTAssertThrowsError(try {
            for index in 0..<UInt16(300) {
                _ = try reassembler.accept(fragFrame(index: index, count: 300, data: chunk))
            }
        }()) {
            guard case .messageTooLarge = $0 as? BuzzLinkReassemblyError else {
                return XCTFail("非预期错误：\($0)")
            }
        }
    }

    func testInterleavedMessagesStayIndependent() throws {
        var reassembler = BuzzLinkMessageReassembler()
        let a0 = fragFrame(index: 0, count: 2, session: 1, data: Data([0xA0]))
        let a1 = fragFrame(index: 1, count: 2, session: 1, data: Data([0xA1]))
        let b0 = fragFrame(index: 0, count: 2, session: 2, data: Data([0xB0]))
        let b1 = fragFrame(index: 1, count: 2, session: 2, data: Data([0xB1]))
        XCTAssertNil(try reassembler.accept(a0))
        XCTAssertNil(try reassembler.accept(b0))
        let completeB = try XCTUnwrap(reassembler.accept(b1))
        XCTAssertEqual(completeB.payload, Data([0xB0, 0xB1]))
        let completeA = try XCTUnwrap(reassembler.accept(a1))
        XCTAssertEqual(completeA.payload, Data([0xA0, 0xA1]))
    }

    func testResetDropsPartials() throws {
        var reassembler = BuzzLinkMessageReassembler()
        XCTAssertNil(try reassembler.accept(fragFrame(index: 0, count: 2, data: Data([0xA0]))))
        reassembler.reset()
        // 重置后只剩第 1 片的消息永远拼不齐；新消息不受影响
        XCTAssertNil(try reassembler.accept(fragFrame(index: 1, count: 2, session: 9, data: Data([0xB1]))))
        let complete = try XCTUnwrap(reassembler.accept(fragFrame(index: 0, count: 2, session: 9, data: Data([0xB0]))))
        XCTAssertEqual(complete.payload, Data([0xB0, 0xB1]))
    }

    // MARK: - 分片发送

    func testFragmenterSmallMessageStaysSingleFrame() throws {
        let message = Data(repeating: 0x01, count: 100)
        let frames = try BuzzLinkFragmenter.fragment(type: 0x11, session: 1, channel: 0x0001, message: message)
        XCTAssertEqual(frames.count, 1)
        XCTAssertFalse(frames[0].isFragment)
        XCTAssertEqual(frames[0].payload, message)
    }

    func testFragmenterExactMultipleOfFragmentData() throws {
        let message = Data(repeating: 0x02, count: 456)
        let frames = try BuzzLinkFragmenter.fragment(type: 0x11, session: 1, channel: 0x0001, message: message)
        XCTAssertEqual(frames.count, 2)
        XCTAssertEqual(frames[1].payload.count, 232)
    }

    func testFragmenterRejectsOversizeMessage() {
        let message = Data(repeating: 0x03, count: BuzzLinkConstants.maxMessageSize + 1)
        XCTAssertThrowsError(try BuzzLinkFragmenter.fragment(type: 0x11, session: 1, channel: 0x0001, message: message)) {
            guard case .messageTooLarge = $0 as? BuzzLinkReassemblyError else {
                return XCTFail("非预期错误：\($0)")
            }
        }
    }

    func testFragmenterRejectsTinyMaxPayload() {
        XCTAssertThrowsError(try BuzzLinkFragmenter.fragment(
            type: 0x11, session: 1, channel: 0x0001,
            message: Data(repeating: 0x04, count: 10), maxPayload: 4
        )) {
            XCTAssertEqual($0 as? BuzzLinkReassemblyError, .invalidMaxPayload(4))
        }
    }

    /// 分片 → 编码 → 字节流解析 → 重组：全链路往返
    func testFragmentEncodeParseReassembleRoundTrip() throws {
        let message = Data((0..<1000).map { UInt8($0 % 241) })
        let fragments = try BuzzLinkFragmenter.fragment(
            type: BuzzLinkFrameType.listResp.rawValue,
            session: 0x55,
            channel: BuzzLinkChannel.control.rawValue,
            message: message
        )
        var stream = Data()
        for fragment in fragments {
            stream.append(try fragment.encoded())
        }
        var parser = BuzzLinkFrameParser()
        parser.append(stream)
        var reassembler = BuzzLinkMessageReassembler()
        var complete: BuzzLinkFrame?
        while let frame = parser.nextFrame() {
            if let done = try reassembler.accept(frame) {
                complete = done
            }
        }
        XCTAssertEqual(parser.discardedFrameCount, 0)
        XCTAssertEqual(complete?.payload, message)
    }
}
