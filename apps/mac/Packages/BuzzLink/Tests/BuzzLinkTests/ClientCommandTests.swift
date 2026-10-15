@testable import BuzzLink
import BuzzLinkCore
import XCTest

/// 命令层行为：session 分配/匹配、JSON 解析、错误映射、超时与乱序（BUZZLINK.md §3.4/§5/§8/§9）。
final class ClientCommandTests: XCTestCase {

    /// 连接好的客户端 + 脚本化传输（响应通过 WeakBox 回灌 inbound）
    private func makeConnected(
        commandTimeout: TimeInterval = 2,
        script: (@Sendable (BuzzLinkFrame, MockTransport) -> Void)? = nil
    ) async throws -> (BuzzLinkClient, MockTransport) {
        let box = WeakBox<MockTransport>()
        let transport = MockTransport { bytes in
            guard let transport = box.value,
                  let frame = try? BuzzLinkFrame.decoded(from: bytes) else { return }
            script?(frame, transport)
        }
        box.value = transport
        let client = BuzzLinkClient(transport: transport, commandTimeout: commandTimeout)
        try await client.connect()
        return (client, transport)
    }

    // MARK: - PING / session

    func testPingPongRoundTrip() async throws {
        let (client, _) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.ping.rawValue else { return }
            XCTAssertEqual(frame.channel, BuzzLinkChannel.link.rawValue)
            if let pong = try? TestResponseBytes.pong(session: frame.session) {
                transport.emitInbound(pong)
            }
        }
        try await client.ping()
        await client.disconnect()
    }

    func testPingResponseDeliveredByteByByte() async throws {
        let (client, _) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.ping.rawValue,
                  let pong = try? TestResponseBytes.pong(session: frame.session) else { return }
            transport.emitInboundByteByByte(pong)
        }
        try await client.ping()
        await client.disconnect()
    }

    func testSessionsIncrementAcrossCommands() async throws {
        let (client, transport) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.ping.rawValue,
                  let pong = try? TestResponseBytes.pong(session: frame.session) else { return }
            transport.emitInbound(pong)
        }
        try await client.ping()
        try await client.ping()
        try await client.ping()
        let sessions = transport.sentFrames.map(\.session)
        XCTAssertEqual(sessions, [1, 2, 3])
        await client.disconnect()
    }

    func testSessionWraparoundSkipsZero() async throws {
        let (client, transport) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.ping.rawValue,
                  let pong = try? TestResponseBytes.pong(session: frame.session) else { return }
            transport.emitInbound(pong)
        }
        await client.setSessionCounterForTest(0xFE)
        try await client.ping()
        try await client.ping()
        let sessions = transport.sentFrames.map(\.session)
        XCTAssertEqual(sessions, [0xFF, 0x01], "0x00 保留给 INFO 推送，回绕须跳过")
        await client.disconnect()
    }

    // MARK: - INFO

    func testGetDeviceInfoParsesJSON() async throws {
        let (client, _) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.infoGet.rawValue,
                  let info = try? TestResponseBytes.info(session: frame.session, deviceName: "BCD-TEST", battery: 66)
            else { return }
            transport.emitInbound(info)
        }
        let info = try await client.getDeviceInfo()
        XCTAssertEqual(info.model, "beecord-sim")
        XCTAssertEqual(info.fwVersion, "0.1.0")
        XCTAssertEqual(info.protocolVersion, 1)
        XCTAssertEqual(info.deviceName, "BCD-TEST")
        XCTAssertEqual(info.battery, 66)
        XCTAssertFalse(info.isCharging)
        XCTAssertEqual(info.storageTotal, 31_000_000)
        XCTAssertEqual(info.storageFree, 12_345_678)
        await client.disconnect()
    }

    func testInfoPushUpdatesStreamAndLastDeviceInfo() async throws {
        let (client, transport) = try await makeConnected()
        let push = try TestResponseBytes.info(session: 0, deviceName: "BCD-PUSH")
        transport.emitInbound(push)

        var iterator = client.deviceInfoUpdates.makeAsyncIterator()
        let pushed = await iterator.next()
        XCTAssertEqual(pushed?.deviceName, "BCD-PUSH")
        let last = await client.lastDeviceInfo
        XCTAssertEqual(last?.deviceName, "BCD-PUSH")
        await client.disconnect()
    }

    // MARK: - LIST / FILE_INFO

    func testListFilesParsesPage() async throws {
        let (client, _) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.listReq.rawValue else { return }
            let entries: [(id: String, durationMs: UInt32, size: Int)] = [
                ("REC_20260103_120000", 5_006, 50_000),
                ("REC_20260102_093000", 30_006, 100_000)
            ]
            if let resp = try? TestResponseBytes.listResp(session: frame.session, total: 5, offset: 0, entries: entries) {
                transport.emitInbound(resp)
            }
        }
        let page = try await client.listFiles(offset: 0, limit: 2)
        XCTAssertEqual(page.total, 5)
        XCTAssertEqual(page.offset, 0)
        XCTAssertEqual(page.files.count, 2)
        XCTAssertEqual(page.files[0].id, "REC_20260103_120000")
        XCTAssertEqual(page.files[0].durationMs, 5_006)
        XCTAssertEqual(page.files[0].size, 50_000)
        await client.disconnect()
    }

    func testListFilesErrorJSONMapsToBadRequest() async throws {
        let (client, _) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.listReq.rawValue,
                  let resp = try? TestResponseBytes.jsonError(
                      type: .listResp, session: frame.session, code: 4_000, message: "bad offset"
                  ) else { return }
            transport.emitInbound(resp)
        }
        do {
            _ = try await client.listFiles(offset: 99)
            XCTFail("越界 offset 应抛 badRequest")
        } catch BuzzLinkError.badRequest {
            // 预期
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        await client.disconnect()
    }

    func testListFilesRejectsNegativeOffset() async throws {
        let (client, transport) = try await makeConnected()
        do {
            _ = try await client.listFiles(offset: -1)
            XCTFail("负 offset 应抛 invalidParameters")
        } catch BuzzLinkError.invalidParameters {
            // 预期
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        XCTAssertTrue(transport.sentFrames.isEmpty, "参数校验失败不应产生任何发送")
        await client.disconnect()
    }

    func testFileInfoFound() async throws {
        let (client, _) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.fileInfoReq.rawValue,
                  let resp = try? TestResponseBytes.fileInfoResp(
                      session: frame.session, id: "REC_20260101_120000", size: 772, crc16: 0x6F3A
                  ) else { return }
            transport.emitInbound(resp)
        }
        let info = try await client.fileInfo(id: "REC_20260101_120000")
        XCTAssertEqual(info.id, "REC_20260101_120000")
        XCTAssertEqual(info.size, 772)
        XCTAssertEqual(info.crc16, 0x6F3A)
        XCTAssertEqual(info.format, "ogg-opus")
        XCTAssertEqual(info.sampleRate, 16_000)
        XCTAssertEqual(info.channels, 1)
        await client.disconnect()
    }

    func testFileInfoNotFoundMapsError() async throws {
        let (client, _) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.fileInfoReq.rawValue,
                  let resp = try? TestResponseBytes.jsonError(
                      type: .fileInfoResp, session: frame.session, code: 4_004, message: "file not found"
                  ) else { return }
            transport.emitInbound(resp)
        }
        do {
            _ = try await client.fileInfo(id: "REC_29990101_000000")
            XCTFail("不存在文件应抛 notFound")
        } catch BuzzLinkError.notFound {
            // 预期
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        await client.disconnect()
    }

    // MARK: - ERROR 帧错误码映射（§9）

    func testErrorFrameCodeMapping() async throws {
        // 状态化脚本：同一 client 顺序应答不同错误码
        final class ScriptState: @unchecked Sendable {
            var code = 0
        }
        let state = ScriptState()
        let (client, _) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.listReq.rawValue,
                  let resp = try? TestResponseBytes.errorFrame(
                      session: frame.session, code: state.code, message: "m"
                  ) else { return }
            transport.emitInbound(resp)
        }
        let cases: [(code: Int, expected: BuzzLinkError)] = [
            (4_000, .badRequest),
            (4_003, .forbidden),
            (4_004, .notFound),
            (4_009, .busy),
            (4_130, .badRequest),
            (4_500, .storage)
        ]
        for testCase in cases {
            state.code = testCase.code
            do {
                _ = try await client.listFiles()
                XCTFail("code \(testCase.code) 应映射为错误")
            } catch let error as BuzzLinkError {
                XCTAssertEqual(error, testCase.expected)
            } catch {
                XCTFail("意外错误类型：\(error)")
            }
        }
        await client.disconnect()
    }

    // MARK: - session 匹配与乱序/迟到

    func testUnknownSessionResponseIgnored() async throws {
        let (client, transport) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.ping.rawValue,
                  let pong = try? TestResponseBytes.pong(session: frame.session) else { return }
            transport.emitInbound(pong)
        }
        // 无等待方的迟到响应：不崩溃、不影响后续命令
        transport.emitInbound(try TestResponseBytes.pong(session: 0x77))
        try await Task.sleep(for: .milliseconds(20))
        try await client.ping()
        let unmatched = await client.unmatchedFrameCount
        XCTAssertEqual(unmatched, 1)
        await client.disconnect()
    }

    func testDuplicateResponseIgnored() async throws {
        let (client, _) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.ping.rawValue,
                  let pong = try? TestResponseBytes.pong(session: frame.session) else { return }
            transport.emitInbound(pong)
            transport.emitInbound(pong)
        }
        try await client.ping()
        // 接收循环异步处理第二帧：轮询至 unmatched 计数到位（上限 2 s）
        var unmatched = 0
        for _ in 0..<200 where unmatched == 0 {
            unmatched = await client.unmatchedFrameCount
            if unmatched == 0 {
                try await Task.sleep(for: .milliseconds(10))
            }
        }
        XCTAssertEqual(unmatched, 1, "重复响应不应再次匹配")
        await client.disconnect()
    }

    func testOutOfOrderResponsesMatchedBySession() async throws {
        // 两并发 LIST：先回第 2 个（session 大）再回第 1 个，结果须各归各。
        // 任务启动顺序不定：以请求 payload 的 offset 定位 session，而非发送顺序。
        let (client, transport) = try await makeConnected()
        let first = Task {
            try await client.listFiles(offset: 0)
        }
        let second = Task {
            try await client.listFiles(offset: 1)
        }
        while transport.sentFrames.count < 2 {
            try await Task.sleep(for: .milliseconds(1))
        }
        struct ListRequest: Decodable { let offset: Int }
        var sessionByOffset: [Int: UInt8] = [:]
        for frame in transport.sentFrames where frame.type == BuzzLinkFrameType.listReq.rawValue {
            if let request = try? JSONDecoder().decode(ListRequest.self, from: frame.payload) {
                sessionByOffset[request.offset] = frame.session
            }
        }
        guard let sessionA = sessionByOffset[0], let sessionB = sessionByOffset[1] else {
            return XCTFail("应发出 offset=0 与 offset=1 两个 LIST_REQ")
        }
        // 先回 B（offset=1 页）再回 A（offset=0 页）
        let pageB = try TestResponseBytes.listResp(
            session: sessionB, total: 3, offset: 1,
            entries: [("REC_20260102_120000", 1_000, 100)]
        )
        let pageA = try TestResponseBytes.listResp(
            session: sessionA, total: 3, offset: 0,
            entries: [("REC_20260103_120000", 2_000, 200)]
        )
        transport.emitInbound(pageB)
        transport.emitInbound(pageA)

        let (resultA, resultB) = try await (first.value, second.value)
        XCTAssertEqual(resultA.offset, 0)
        XCTAssertEqual(resultA.files.first?.id, "REC_20260103_120000")
        XCTAssertEqual(resultB.offset, 1)
        XCTAssertEqual(resultB.files.first?.id, "REC_20260102_120000")
        await client.disconnect()
    }

    func testWrongTypeResponseThrowsProtocolViolation() async throws {
        let (client, _) = try await makeConnected { frame, transport in
            // 用 PONG 应答 LIST_REQ：session 匹配但 type 错误
            guard frame.type == BuzzLinkFrameType.listReq.rawValue,
                  let pong = try? TestResponseBytes.pong(session: frame.session) else { return }
            transport.emitInbound(pong)
        }
        do {
            _ = try await client.listFiles()
            XCTFail("错误 type 响应应抛 protocolViolation")
        } catch BuzzLinkError.protocolViolation {
            // 预期
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        await client.disconnect()
    }

    // MARK: - 超时

    func testCommandTimeoutThrowsAndStateRecovers() async throws {
        // LIST 无响应 → 超时；PING 有响应 → 正常（waiter 已清理、状态仍 ready）
        let (client, _) = try await makeConnected(commandTimeout: 0.05) { frame, transport in
            guard frame.type == BuzzLinkFrameType.ping.rawValue,
                  let pong = try? TestResponseBytes.pong(session: frame.session) else { return }
            transport.emitInbound(pong)
        }
        do {
            _ = try await client.listFiles()
            XCTFail("无响应应抛 commandTimeout")
        } catch BuzzLinkError.commandTimeout {
            // 预期
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        let state = await client.state
        XCTAssertEqual(state, .ready, "超时后状态应回 ready，不影响后续命令")
        try await client.ping()
        await client.disconnect()
    }

    func testLateResponseAfterTimeoutLandsUnmatched() async throws {
        let (client, transport) = try await makeConnected(commandTimeout: 0.05) { frame, transport in
            guard frame.type == BuzzLinkFrameType.ping.rawValue,
                  let pong = try? TestResponseBytes.pong(session: frame.session) else { return }
            transport.emitInbound(pong)
        }
        do {
            _ = try await client.listFiles()
            XCTFail("应超时")
        } catch BuzzLinkError.commandTimeout {
            // 预期
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        // 迟到响应（session=1）落到 unmatched，不污染下一次匹配
        transport.emitInbound(try TestResponseBytes.listResp(session: 1, total: 0, offset: 0, entries: []))
        try await Task.sleep(for: .milliseconds(20))
        let unmatched = await client.unmatchedFrameCount
        XCTAssertEqual(unmatched, 1)

        // 新命令使用新 session，正常完成
        try await client.ping()
        await client.disconnect()
    }
}
