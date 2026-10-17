import BuzzLinkCore
import XCTest

/// Ogg 页构造器：传入**原始 lacing 段序列**（每段 ≤255 字节；段长 255 表示该分包跨页
/// 待续，恰 255 的分包需显式补 0 长段收尾——由调用方表达），计算 Ogg CRC32。
private enum OggPageMaker {
    static func page(
        serial: UInt32 = 7,
        sequence: UInt32,
        headerType: UInt8,
        segments: [Data],
        granule: UInt64 = 0,
        version: UInt8 = 0
    ) -> Data {
        var segTable: [UInt8] = []
        var payload = Data()
        for segment in segments {
            precondition(segment.count <= 255, "段长超 255 须由调用方拆分续页")
            segTable.append(UInt8(segment.count))
            payload.append(segment)
        }
        var page = Data()
        page.append(contentsOf: [0x4F, 0x67, 0x67, 0x53]) // "OggS"
        page.append(version)
        page.append(headerType)
        appendLE(&page, value: granule, bytes: 8)
        appendLE(&page, value: UInt64(serial), bytes: 4)
        appendLE(&page, value: UInt64(sequence), bytes: 4)
        page.append(contentsOf: [0, 0, 0, 0]) // CRC 占位（计算时置零）
        page.append(UInt8(segTable.count))
        page.append(contentsOf: segTable)
        page.append(payload)
        let crc = oggCRC(page)
        page[22] = UInt8(crc & 0xFF)
        page[23] = UInt8((crc >> 8) & 0xFF)
        page[24] = UInt8((crc >> 16) & 0xFF)
        page[25] = UInt8((crc >> 24) & 0xFF)
        return page
    }

    static func appendLE(_ data: inout Data, value: UInt64, bytes: Int) {
        for index in 0..<bytes {
            data.append(UInt8((value >> (8 * index)) & 0xFF))
        }
    }

    /// Ogg CRC32（与 OggDemuxer 同算法，独立实现便于交叉验证）
    static func oggCRC(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0
        for (position, byte) in data.enumerated() {
            let value: UInt8 = (position >= 22 && position <= 25) ? 0 : byte
            crc = (crc << 8) ^ table[Int(UInt8((crc >> 24) ^ UInt32(value)))]
        }
        return crc
    }

    private static let table: [UInt32] = {
        (0..<256).map { index in
            var crc = UInt32(index) << 24
            for _ in 0..<8 {
                crc = (crc & 0x8000_0000) != 0 ? (crc << 1) ^ 0x04C1_1DB7 : crc << 1
            }
            return crc
        }
    }()
}

final class OggDemuxerTests: XCTestCase {

    private let opusHead = Data([0x4F, 0x70, 0x75, 0x73, 0x48, 0x65, 0x61, 0x64]) // "OpusHead"

    func testParsesValidMultiPageFile() throws {
        let file = OggPageMaker.page(sequence: 0, headerType: 0x02, segments: [opusHead])
            + OggPageMaker.page(sequence: 1, headerType: 0, segments: [Data([1, 2, 3])])
            + OggPageMaker.page(sequence: 2, headerType: 0x04, segments: [Data([4, 5])], granule: 960)
        let pages = try OggDemuxer.pages(data: file)
        XCTAssertEqual(pages.count, 3)
        XCTAssertTrue(pages[0].isBeginningOfStream)
        XCTAssertTrue(pages[2].isEndOfStream)
        XCTAssertEqual(pages[2].granulePosition, 960)
        XCTAssertEqual(pages[1].payload, Data([1, 2, 3]))
        XCTAssertEqual(pages[2].sequence, 2)
    }

    func testExtractOpusPacketsReassemblesContinuation() throws {
        // 一个 300 字节分包：前 255 在 page1 末段，后 45 在 page2 首段（续页重组）
        let part1 = Data(repeating: 0xAB, count: 255)
        let part2 = Data(repeating: 0xCD, count: 45)
        let file = OggPageMaker.page(sequence: 0, headerType: 0x02, segments: [opusHead])
            + OggPageMaker.page(sequence: 1, headerType: 0, segments: [Data([9]), part1])
            + OggPageMaker.page(sequence: 2, headerType: 0x01, segments: [part2]) // 续页
            + OggPageMaker.page(sequence: 3, headerType: 0x04, segments: [Data([7])])
        let packets = try OggDemuxer.extractOpusPackets(data: file)
        XCTAssertEqual(packets.count, 4) // ID + 9 + 300字节重组 + 7
        XCTAssertEqual(packets[0], opusHead)
        XCTAssertEqual(packets[1], Data([9]))
        XCTAssertEqual(packets[2].count, 300)
        XCTAssertEqual(packets[2].first, 0xAB)
        XCTAssertEqual(packets[2].last, 0xCD)
        XCTAssertEqual(packets[3], Data([7]))
    }

    func testSequenceGapDetected() {
        let file = OggPageMaker.page(sequence: 0, headerType: 0x02, segments: [opusHead])
            + OggPageMaker.page(sequence: 2, headerType: 0x04, segments: [Data([1])])
        XCTAssertThrowsError(try OggDemuxer.pages(data: file)) { error in
            guard case OggDemuxer.OggDemuxError.sequenceGap(let serial, let expected, let actual) = error else {
                return XCTFail("期望 sequenceGap，实际 \(error)")
            }
            XCTAssertEqual(serial, 7)
            XCTAssertEqual(expected, 1)
            XCTAssertEqual(actual, 2)
        }
    }

    func testUnsupportedVersionRejected() {
        let file = OggPageMaker.page(sequence: 0, headerType: 0x02, segments: [opusHead], version: 1)
            + OggPageMaker.page(sequence: 1, headerType: 0x04, segments: [Data([1])])
        XCTAssertThrowsError(try OggDemuxer.pages(data: file)) { error in
            guard case OggDemuxer.OggDemuxError.unsupportedVersion(let version) = error else {
                return XCTFail("期望 unsupportedVersion，实际 \(error)")
            }
            XCTAssertEqual(version, 1)
        }
    }

    func testTruncatedTailThrows() {
        let file = OggPageMaker.page(sequence: 0, headerType: 0x02, segments: [opusHead])
            + OggPageMaker.page(sequence: 1, headerType: 0, segments: [Data(repeating: 1, count: 64)])
        let truncated = file.prefix(file.count - 8)
        XCTAssertThrowsError(try OggDemuxer.pages(data: truncated)) { error in
            guard case OggDemuxer.OggDemuxError.truncatedPage = error else {
                return XCTFail("期望 truncatedPage，实际 \(error)")
            }
        }
    }

    func testEmptyFileThrowsMissingBOS() {
        XCTAssertThrowsError(try OggDemuxer.pages(data: Data())) { error in
            guard case OggDemuxer.OggDemuxError.missingBeginningOfStream = error else {
                return XCTFail("期望 missingBeginningOfStream，实际 \(error)")
            }
        }
    }

    func testCorruptedPayloadFailsChecksum() {
        var file = OggPageMaker.page(sequence: 0, headerType: 0x02, segments: [opusHead])
            + OggPageMaker.page(sequence: 1, headerType: 0x04, segments: [Data([1, 2, 3])])
        // 翻转 page2 的首个 payload 字节（CRC 随之失配）
        let payloadOffset = file.count - 3
        file[payloadOffset] ^= 0xFF
        XCTAssertThrowsError(try OggDemuxer.pages(data: file)) { error in
            guard case OggDemuxer.OggDemuxError.pageChecksumMismatch = error else {
                return XCTFail("期望 pageChecksumMismatch，实际 \(error)")
            }
        }
    }
}
