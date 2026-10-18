import BuzzLink
import BuzzLinkCore
import XCTest

/// FrameTapTransport 单测（T7 Phase A）：TX/RX 方向标注、粘包/拆包重组（与 Client 同 parser
/// 语义）、断连 reset 后重连首帧不混淆、state 透传。
final class FrameTapTransportTests: XCTestCase {

    /// 状态收集器（跨线程安全）
    private final class StateCollector: @unchecked Sendable {
        private let lock = NSLock()
        private var storage: [BuzzLinkTransportState] = []
        var values: [BuzzLinkTransportState] { lock.withLock { storage } }
        func append(_ state: BuzzLinkTransportState) { lock.withLock { storage.append(state) } }
    }

    /// 事件收集器（跨线程安全）
    private final class Collector: @unchecked Sendable {
        private let lock = NSLock()
        private var storage: [FrameTapEvent] = []
        var values: [FrameTapEvent] { lock.withLock { storage } }
        var frames: [BuzzLinkFrame] { lock.withLock { storage.map(\.frame) } }
        func append(_ event: FrameTapEvent) { lock.withLock { storage.append(event) } }
    }

    private func collect(_ tap: FrameTapTransport) -> (Collector, Task<Void, Never>) {
        let collector = Collector()
        let task = Task {
            for await event in tap.events {
                collector.append(event)
            }
        }
        return (collector, task)
    }

    private func frame(_ type: UInt8, session: UInt8) throws -> (BuzzLinkFrame, Data) {
        let f = BuzzLinkFrame(type: type, session: session, channel: 0x0001,
                              payload: Data([type, 0xAA]))
        return (f, try f.encoded())
    }

    // 1. TX 方向标注：send 一帧 → 事件 tx + frame 逐字段相等 + 原始字节一致
    func testTXDirectionAndFrameEquality() async throws {
        let mock = MockTransport()
        let tap = FrameTapTransport(wrapping: mock)
        let (collector, task) = collect(tap)
        defer { task.cancel() }

        let (ping, bytes) = try frame(0x01, session: 0x21)
        try await tap.send(bytes)

        let events = await pollUntil(timeout: 2) { !collector.values.isEmpty }
        XCTAssertTrue(events, "TX 事件应到达")
        let event = collector.values[0]
        XCTAssertEqual(event.direction, .tx)
        XCTAssertEqual(event.frame, ping)
        XCTAssertEqual(event.rawBytes, bytes)
        XCTAssertEqual(try BuzzLinkFrame.decoded(from: event.rawBytes), ping, "rawBytes 可解码回同帧")
    }

    // 2. RX 粘包：两帧合并一个片段 → 两个事件、顺序与方向正确
    func testRXCoalescedFrames() async throws {
        let mock = MockTransport()
        let tap = FrameTapTransport(wrapping: mock)
        let (collector, task) = collect(tap)
        defer { task.cancel() }

        let (a, bytesA) = try frame(0x03, session: 0x01)
        let (b, bytesB) = try frame(0x04, session: 0x02)
        mock.emitInbound(bytesA + bytesB)

        let ok = await pollUntil(timeout: 2) { collector.values.count == 2 }
        XCTAssertTrue(ok, "粘包应重组为两个 RX 事件")
        XCTAssertEqual(collector.frames, [a, b])
        XCTAssertTrue(collector.values.allSatisfy { $0.direction == .rx })
    }

    // 3. RX 拆包：一帧逐字节喂入 → 完整帧事件（§3.3 字节流重组）
    func testRXFragmentedFrame() async throws {
        let mock = MockTransport()
        let tap = FrameTapTransport(wrapping: mock)
        let (collector, task) = collect(tap)
        defer { task.cancel() }

        let (info, bytes) = try frame(0x03, session: 0x00)
        mock.emitInboundByteByByte(bytes)

        let ok = await pollUntil(timeout: 2) { !collector.values.isEmpty }
        XCTAssertTrue(ok)
        XCTAssertEqual(collector.values[0].frame, info)
        XCTAssertEqual(collector.values[0].direction, .rx)
    }

    // 4. 断连→重连全流程（端到端确定性；半包微观语义由双点 reset + 锁串行保证）：
    // 完整旧帧流通 → 断连（reset 已执行）→ connect 再 reset（新会话边界）→
    // 新帧干净解析；首帧不与旧会话字节混淆、无畸形帧。
    func testReconnectFlowYieldsCleanFrames() async throws {
        let mock = MockTransport()
        let tap = FrameTapTransport(wrapping: mock)
        let (collector, task) = collect(tap)
        defer { task.cancel() }
        let stateCollector = StateCollector()
        let stateTask = Task {
            for await state in tap.stateStream {
                stateCollector.append(state)
            }
        }
        defer { stateTask.cancel() }

        // 步骤 1：断连前完整旧帧正常流通
        let (old, oldBytes) = try frame(0x11, session: 0x40)
        mock.emitInbound(oldBytes)
        let oldSeen = await pollUntil(timeout: 2) { collector.frames.contains(old) }
        XCTAssertTrue(oldSeen, "断连前完整帧应正常收到")

        // 步骤 2：断连——等状态转发可见（reset 在转发前同步执行）
        mock.emitState(.disconnected)
        let disconnectedSeen = await pollUntil(timeout: 2) {
            stateCollector.values.contains(.disconnected)
        }
        XCTAssertTrue(disconnectedSeen, "断连状态经转发流可见（reset 已执行）")

        // 步骤 3：重连（connect 内再次 reset——新会话边界，幂等）
        try await tap.connect()

        // 步骤 4：新帧干净解析；不与旧会话字节混淆、不产畸形帧
        let (fresh, freshBytes) = try frame(0x02, session: 0x41)
        mock.emitInbound(freshBytes)
        let settled = await pollUntil(timeout: 2) { collector.frames.count >= 2 }
        XCTAssertTrue(settled)
        XCTAssertEqual(collector.frames, [old, fresh], "重连后仅新会话帧，无混淆无畸形")
        XCTAssertTrue(collector.values.allSatisfy { $0.direction == .rx })
    }

    // 5. state 透传 + 断连后 inbound 继续可用（重连重下场景）
    func testStatePassthroughAndRXAfterReconnect() async throws {
        let mock = MockTransport()
        let tap = FrameTapTransport(wrapping: mock)

        let stateCollector = StateCollector()
        let stateTask = Task {
            for await state in tap.stateStream {
                stateCollector.append(state)
            }
        }
        defer { stateTask.cancel() }

        mock.emitState(.connecting)
        mock.emitState(.ready)
        mock.emitState(.disconnected)
        let statesOK = await pollUntil(timeout: 2) { stateCollector.values.count >= 3 }
        XCTAssertTrue(statesOK)
        XCTAssertEqual(Array(stateCollector.values.prefix(3)), [.connecting, .ready, .disconnected],
                       "state 原样透传")

        // 断连后再次喂入完整帧：reset 之后 parser 干净，正常重组
        let (collector, task) = collect(tap)
        defer { task.cancel() }
        let (pong, bytes) = try frame(0x02, session: 0x42)
        mock.emitInbound(bytes)
        let ok = await pollUntil(timeout: 2) { !collector.values.isEmpty }
        XCTAssertTrue(ok)
        XCTAssertEqual(collector.values[0].frame, pong)
    }
}
