import BuzzLinkCore
import XCTest

/// FrameLogBuffer 单测（T7 Phase A）：环形淘汰/行 ID 单调/三过滤组合/向量导出 round-trip。
final class FrameLogBufferTests: XCTestCase {

    /// 可控时钟（ms 精度步进）；类盒避免 @Sendable 闭包捕获可变变量。
    private final class ClockBox: @unchecked Sendable {
        var now: TimeInterval = 1_000
        func advance() { now += 0.001 }      // 每次 +1ms
    }

    private func frame(_ type: UInt8, session: UInt8 = 1, payload: [UInt8] = []) -> BuzzLinkFrame {
        BuzzLinkFrame(type: type, session: session, channel: 0x0001, payload: Data(payload))
    }

    // 1. 超容量淘汰最旧
    func testEvictsOldestBeyondCapacity() {
        var buffer = FrameLogBuffer()
        let overflow = 7
        for i in 0..<(FrameLogBuffer.capacity + overflow) {
            buffer.append(frame: frame(0x01, session: UInt8(i & 0xFF)), direction: .tx)
        }
        XCTAssertEqual(buffer.count, FrameLogBuffer.capacity)
        XCTAssertEqual(buffer.entries.first?.frame.session, UInt8(overflow & 0xFF),
                       "最早 \(overflow) 条应被淘汰")
        XCTAssertEqual(buffer.entries.last?.frame.session, UInt8((FrameLogBuffer.capacity + overflow - 1) & 0xFF))
    }

    // 2. 行 ID 单调、淘汰不重用
    func testRowIDMonotonicAcrossEviction() {
        var buffer = FrameLogBuffer()
        for i in 0..<(FrameLogBuffer.capacity + 100) {
            buffer.append(frame: frame(0x01, session: UInt8(i & 0xFF)), direction: .tx)
        }
        let ids = buffer.entries.map(\.rowID)
        XCTAssertEqual(Set(ids).count, ids.count, "行 ID 无重复")
        XCTAssertTrue(ids.zipWithNext().allSatisfy { $0.1 > $0.0 }, "行 ID 严格递增")
        XCTAssertEqual(buffer.pendingRowID, FrameLogBuffer.capacity + 100)
    }

    // 3. 方向过滤
    func testDirectionFilter() {
        var buffer = FrameLogBuffer()
        buffer.append(frame: frame(0x01), direction: .tx)
        buffer.append(frame: frame(0x02), direction: .rx)
        buffer.append(frame: frame(0x01), direction: .tx)
        XCTAssertEqual(buffer.filtered(FrameLogBuffer.Filter(direction: .tx)).count, 2)
        XCTAssertEqual(buffer.filtered(FrameLogBuffer.Filter(direction: .rx)).count, 1)
        XCTAssertEqual(buffer.filtered(FrameLogBuffer.Filter()).count, 3)
    }

    // 4. type 多选并集
    func testTypeFilterUnion() {
        var buffer = FrameLogBuffer()
        buffer.append(frame: frame(0x10), direction: .tx)
        buffer.append(frame: frame(0x11), direction: .rx)
        buffer.append(frame: frame(0x32), direction: .rx)
        buffer.append(frame: frame(0x34), direction: .rx)
        XCTAssertEqual(buffer.filtered(FrameLogBuffer.Filter(types: [0x10, 0x11])).count, 2)
        XCTAssertEqual(buffer.filtered(FrameLogBuffer.Filter(types: [0x32, 0x34])).count, 2)
        XCTAssertEqual(buffer.filtered(FrameLogBuffer.Filter(types: [0x10, 0x32, 0x34])).count, 3)
        XCTAssertEqual(buffer.filtered(FrameLogBuffer.Filter(types: [])).count, 4, "空集 = 全部")
    }

    // 5. 文本过滤（type 名/payload/注释）+ 三过滤 AND 组合
    func testTextFilterAndComposition() {
        var buffer = FrameLogBuffer()
        buffer.append(frame: frame(0x10, payload: [0x7B, 0x22, 0x6F, 0x66, 0x66]), direction: .tx, comment: "分页拉列表")
        buffer.append(frame: frame(0x11, payload: [0x7B, 0x22, 0x74]), direction: .rx, comment: "status=0")
        // 文本命中 type 名
        XCTAssertEqual(buffer.filtered(FrameLogBuffer.Filter(text: "list_req")).count, 1)
        // 文本命中 payload hex（7b22 公共子串 → 两条）
        XCTAssertEqual(buffer.filtered(FrameLogBuffer.Filter(text: "7b22")).count, 2)
        // 文本命中注释
        XCTAssertEqual(buffer.filtered(FrameLogBuffer.Filter(text: "status")).count, 1)
        // AND 组合：方向 rx + type 0x11 + 文本 7b22 → 1 条
        XCTAssertEqual(buffer.filtered(FrameLogBuffer.Filter(direction: .rx, types: [0x11], text: "7b22")).count, 1)
        // AND 组合：方向 tx + type 0x11 → 0 条
        XCTAssertEqual(buffer.filtered(FrameLogBuffer.Filter(direction: .tx, types: [0x11], text: "")).count, 0)
    }

    // 6. 导出 round-trip 逐字段相等；comment 补 type 名；ts 为相对毫秒
    func testExportVectorRoundTrip() throws {
        let box = ClockBox()
        // 注入可控时钟：重建 buffer（append 内首条标记 t0）
        var controlled = FrameLogBuffer(clock: { box.now })
        box.advance()                           // t0 → 第 1ms
        controlled.append(frame: frame(0x10, session: 0x21, payload: [0x7B, 0x7D]), direction: .tx, comment: "分页拉列表")
        box.advance(); box.advance()
        controlled.append(frame: frame(0x11, session: 0x21, payload: [0x30]), direction: .rx)
        let vector = controlled.exportVector(comment: "测试向量")
        // 编码 → 解码 round-trip（schema 校验 + 字段逐位相等）
        let data = try JSONEncoder().encode(vector)
        let decoded = try JSONDecoder().decode(BuzzLinkTestVector.self, from: data)
        XCTAssertEqual(decoded.schema, "buzzlink-vector/1")
        XCTAssertEqual(decoded.entries.count, 2)
        let first = decoded.entries[0]
        XCTAssertEqual(first.direction, .tx)
        XCTAssertEqual(first.type, 0x10)
        XCTAssertEqual(first.session, 0x21)
        XCTAssertEqual(first.channel, 0x0001)
        XCTAssertEqual(try first.decodedPayload(), Data([0x7B, 0x7D]))
        XCTAssertEqual(first.comment, "LIST_REQ · 分页拉列表", "comment 补 type 名")
        XCTAssertEqual(decoded.entries[1].comment, "LIST_RESP")
        XCTAssertEqual(first.ts ?? -1, 0, accuracy: 0.001, "首条 ts = 0")
        XCTAssertEqual(decoded.entries[1].ts ?? -1, 2, accuracy: 0.001, "次条 ts = 2ms")
    }
}

private extension Array {
    func zipWithNext() -> [(Element, Element)] {
        zip(self, dropFirst()).map { ($0, $1) }
    }
}
