import XCTest
@testable import BuzzLinkCore

/// CRC-16/CCITT-FALSE 自测向量（BUZZLINK.md §3.2）
final class CRC16Tests: XCTestCase {

    func testCheckVectorDigits() {
        XCTAssertEqual(CRC16.ccittFalse(Data("123456789".utf8)), 0x29B1)
    }

    func testCheckVectorProjectName() {
        XCTAssertEqual(CRC16.ccittFalse(Data("Beecord".utf8)), 0x5D1E)
    }

    func testEmptyInputKeepsSeed() {
        XCTAssertEqual(CRC16.ccittFalse(Data()), 0xFFFF)
        XCTAssertEqual(CRC16.ccittFalse(Data(), updating: 0x1234), 0x1234)
    }

    func testIncrementalMatchesOneShot() {
        let data = Data("123456789".utf8)
        var crc = CRC16.ccittFalse(data.prefix(4))
        crc = CRC16.ccittFalse(data.suffix(5), updating: crc)
        XCTAssertEqual(crc, 0x29B1)
    }

    func testAcceptsSlicesAndArrays() {
        let array: [UInt8] = Array("123456789".utf8)
        XCTAssertEqual(CRC16.ccittFalse(array), 0x29B1)
        XCTAssertEqual(CRC16.ccittFalse(Data(array).prefix(9)), 0x29B1)
    }
}
