import BuzzLink
import BuzzLinkCore
import XCTest

/// `BuzzLinkClient.sendRaw` 单测（T7 Phase A）：session 覆写与响应匹配、fire-and-forget、
/// 超时路径、发送期断连错误映射、门控（未连接抛 notConnected；`requireCommandAllowed`
/// 与高层命令同源——下载中放行语义由 SyncEngine 队列用例间接覆盖，见 SyncEngineAutoSyncTests）。
final class SendRawTests: XCTestCase {

    /// 弱引用盒：onSend 闭包回灌 transport.inbound（同 MockTransport.swift 惯例）
    private final class WeakBox<Value: AnyObject>: @unchecked Sendable {
        weak var value: Value?
        init(_ value: Value? = nil) { self.value = value }
    }



    // 1. awaitResponse：session 覆写 + 同 session 响应匹配，返回响应帧
    func testSendRawMatchesResponseByAllocatedSession() async throws {
        let box = WeakBox<MockTransport>()
        let transport = MockTransport { [box] bytes in
            guard let request = try? BuzzLinkFrame.decoded(from: bytes),
                  let response = try? pongResponse(to: request),
                  let encoded = try? response.encoded() else { return }
            box.value?.emitInbound(encoded)
        }
        box.value = transport
        let client = BuzzLinkClient(transport: transport)
        try await client.connect()

        let callerFrame = BuzzLinkFrame(type: 0x01, session: 0xEE, channel: 0x0000)
        let response = try await client.sendRaw(callerFrame)
        XCTAssertEqual(response?.type, 0x02, "应收到 PONG 响应")
        XCTAssertNotEqual(response?.session, 0xEE, "session 由客户端分配，覆写调用方值")
        // 响应与请求同 session 才匹配成功（收到响应即内部配对成功）+ 发出的帧可复核：
        XCTAssertEqual(transport.sentFrames.first?.session, response?.session,
                       "发出的请求与收到的响应同 session")
        await client.disconnect()
    }

    // 2. fire-and-forget：awaitResponse=false 立即返回 nil 且帧已发出（session 已覆写）
    func testSendRawFireAndForget() async throws {
        let transport = MockTransport { _ in }
        let client = BuzzLinkClient(transport: transport)
        try await client.connect()

        let frame = BuzzLinkFrame(type: 0x04, session: 0x77, channel: 0x0001)
        let result = try await client.sendRaw(frame, awaitResponse: false)
        XCTAssertNil(result)
        XCTAssertEqual(transport.sentFrames.count, 1)
        XCTAssertNotEqual(transport.sentFrames[0].session, 0x77, "发出的帧 session 已被覆写")
        await client.disconnect()
    }

    // 3. 超时：对端静默不回包 → commandTimeout（注入短超时）
    func testSendRawTimeout() async throws {
        let transport = MockTransport { _ in /* 静默：不回包 */ }
        let client = BuzzLinkClient(transport: transport, commandTimeout: 0.2)
        try await client.connect()
        let frame = BuzzLinkFrame(type: 0x01, session: 0, channel: 0x0000)
        do {
            _ = try await client.sendRaw(frame)
            XCTFail("应抛 commandTimeout")
        } catch BuzzLinkError.commandTimeout {
            // 期望路径
        }
        await client.disconnect()
    }

    // 4. 发送期断连：错误映射 .disconnected 且状态收敛（stateStream 幂等去重路径）
    func testSendRawMapsDisconnectDuringSend() async throws {
        let transport = MockTransport { _ in }
        let client = BuzzLinkClient(transport: transport)
        try await client.connect()
        transport.failSends()   // 后续 send 抛错 + 发断连状态
        let frame = BuzzLinkFrame(type: 0x01, session: 0, channel: 0x0000)
        do {
            _ = try await client.sendRaw(frame)
            XCTFail("应抛 disconnected")
        } catch BuzzLinkError.disconnected {
            // 期望路径
        }
        // 断连收敛：此时再发应抛 notConnected（hasEverBeenReady 会话断连后重连策略由上层决定，
        // 此处只验证错误映射与即时收敛不崩溃）
        do {
            _ = try await client.sendRaw(frame, awaitResponse: false)
            XCTFail("断连后应抛 notConnected/invalidState")
        } catch {
            // 任意门控错误均可接受（notConnected / disconnected）
        }
        await client.disconnect()
    }

    // 5. 门控：未连接（idle）调用抛 notConnected（requireCommandAllowed 被应用）
    func testSendRawRequiresConnection() async throws {
        let transport = MockTransport { _ in }
        let client = BuzzLinkClient(transport: transport)   // 不 connect
        let frame = BuzzLinkFrame(type: 0x01, session: 0, channel: 0x0000)
        do {
            _ = try await client.sendRaw(frame)
            XCTFail("未连接应抛 notConnected")
        } catch BuzzLinkError.notConnected {
            // 期望路径
        }
    }
}

// MARK: - 文件级辅助

/// PING 的 mock 回包（同 session PONG）。
private func pongResponse(to request: BuzzLinkFrame) throws -> BuzzLinkFrame {
    BuzzLinkFrame(type: 0x02, session: request.session, channel: request.channel,
                  payload: request.payload)
}
