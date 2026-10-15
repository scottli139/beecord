import BuzzLink
import BuzzLinkCore
import XCTest

/// 客户端状态机测试（studio-tech-design §4.2：idle → scanning → connecting → ready →
/// downloading → verifying → ready，断连收敛 disconnected）。
final class ClientStateMachineTests: XCTestCase {

    func testInitialStateIsIdle() async {
        let client = BuzzLinkClient(transport: MockTransport())
        let state = await client.state
        XCTAssertEqual(state, .idle)
    }

    func testConnectEmitsReadyAndDisconnectEmitsDisconnected() async throws {
        let transport = MockTransport()
        let client = BuzzLinkClient(transport: transport)

        let collector = ClientStateCollector()
        let collecting = Task {
            for await state in client.stateUpdates {
                collector.append(state)
            }
        }

        try await client.connect()
        let readyState = await client.state
        XCTAssertEqual(readyState, .ready)

        await client.disconnect()
        let disconnectedState = await client.state
        XCTAssertEqual(disconnectedState, .disconnected)

        // 状态流经观察循环异步投递：有界排空后再断言
        for _ in 0..<200 where collector.values.count < 3 {
            try await Task.sleep(for: .milliseconds(10))
        }
        collecting.cancel()
        XCTAssertEqual(collector.values, [.connecting, .ready, .disconnected])
    }

    func testConnectFailureConvergesToDisconnected() async {
        struct FakeError: Error {}
        let transport = MockTransport(connectError: FakeError())
        let client = BuzzLinkClient(transport: transport)

        do {
            try await client.connect()
            XCTFail("connect 应抛出传输错误")
        } catch {
            let state = await client.state
            XCTAssertEqual(state, .disconnected)
        }
    }

    func testDoubleConnectThrowsInvalidState() async throws {
        let transport = MockTransport()
        let client = BuzzLinkClient(transport: transport)
        try await client.connect()
        do {
            try await client.connect()
            XCTFail("重复 connect 应抛 invalidState")
        } catch BuzzLinkError.invalidState {
            // 预期
        }
    }

    func testReconnectAfterDisconnect() async throws {
        let transport = MockTransport()
        let client = BuzzLinkClient(transport: transport)
        try await client.connect()
        await client.disconnect()
        let disconnected = await client.state
        XCTAssertEqual(disconnected, .disconnected)

        try await client.connect()
        let ready = await client.state
        XCTAssertEqual(ready, .ready)
        await client.disconnect()
    }

    func testCommandBeforeConnectThrowsNotConnected() async {
        let client = BuzzLinkClient(transport: MockTransport())
        do {
            _ = try await client.listFiles()
            XCTFail("未连接时 listFiles 应抛 notConnected")
        } catch BuzzLinkError.notConnected {
            // 预期
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
    }

    func testDisconnectFailsPendingCommand() async throws {
        let transport = MockTransport()
        let client = BuzzLinkClient(transport: transport)
        try await client.connect()

        let pingTask = Task {
            try await client.ping()
        }
        // 等 PING 发出后断连
        while transport.sentFrames.isEmpty {
            try await Task.sleep(for: .milliseconds(1))
        }
        transport.failSends()

        do {
            try await pingTask.value
            XCTFail("断连后 PING 应抛 disconnected")
        } catch BuzzLinkError.disconnected {
            // 预期
        }
        let state = await client.state
        XCTAssertEqual(state, .disconnected)
    }

    func testDownloadBeforeConnectThrowsNotConnected() async {
        let client = BuzzLinkClient(transport: MockTransport())
        do {
            _ = try await client.download(fileID: "REC_20260101_120000")
            XCTFail("未连接时 download 应抛 notConnected")
        } catch BuzzLinkError.notConnected {
            // 预期
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
    }
}
