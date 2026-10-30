@testable import BuzzLink
import BuzzLinkCore
import XCTest

/// TIME_SET 对时（BUZZLINK.md v0.2 §8.4）：请求编码、响应严格解析、
/// 回显校验、设备错误映射、参数范围。
final class ClientTimeSetTests: XCTestCase {

    /// TIME_SET_RESP 帧编码（文件级，规避 @Sendable 脚本闭包捕获 self）
    static func timeSetRespBytes(session: UInt8, json: String) -> Data {
        let payload = Data(json.utf8)
        let frame = BuzzLinkFrame(
            type: BuzzLinkFrameType.timeSetResp.rawValue,
            session: session,
            channel: BuzzLinkChannel.control.rawValue,
            payload: payload
        )
        return try! frame.encoded()
    }

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

    func testTimeSetSuccessEchoesEpoch() async throws {
        let (client, _) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.timeSetReq.rawValue else { return }
            XCTAssertEqual(frame.channel, BuzzLinkChannel.control.rawValue)
            let body = String(decoding: frame.payload, as: UTF8.self)
            XCTAssertTrue(body.contains("\"epochMs\":1704067200000"), body)
            XCTAssertTrue(body.contains("\"utcOffsetMin\":480"), body)
            transport.emitInbound(Self.timeSetRespBytes(
                session: frame.session,
                json: #"{"status":"ok","epochMs":1704067200000}"#))
        }
        try await client.setTime(epochMs: 1_704_067_200_000, utcOffsetMin: 480)
        await client.disconnect()
    }

    func testTimeSetEchoMismatchThrows() async throws {
        let (client, _) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.timeSetReq.rawValue else { return }
            transport.emitInbound(Self.timeSetRespBytes(
                session: frame.session,
                json: #"{"status":"ok","epochMs":1704067200001}"#))
        }
        do {
            try await client.setTime(epochMs: 1_704_067_200_000, utcOffsetMin: 0)
            XCTFail("回显不符应抛 protocolViolation")
        } catch BuzzLinkError.protocolViolation {
            // 预期
        }
        await client.disconnect()
    }

    func testTimeSetDeviceErrorMapped() async throws {
        // 设备侧拒绝（如 RTC 写失败 4999）：epoch 在窗口内（客户端预校验放行），
        // 错误映射走 mapErrorCode → internalFailure
        let (client, _) = try await makeConnected { frame, transport in
            guard frame.type == BuzzLinkFrameType.timeSetReq.rawValue else { return }
            transport.emitInbound(Self.timeSetRespBytes(
                session: frame.session,
                json: #"{"status":"error","code":4999,"message":"rtc write failed"}"#))
        }
        do {
            try await client.setTime(epochMs: 1_704_067_200_000, utcOffsetMin: 0)
            XCTFail("设备拒绝应抛 internalFailure")
        } catch BuzzLinkError.internalFailure(let msg) {
            XCTAssertTrue(msg.contains("rtc write failed"), msg)
        }
        await client.disconnect()
    }

    func testTimeSetEpochOutOfRangeRejectedLocally() async throws {
        // §8.4 epoch 窗口预校验（2024-01-01 之前不上空口）
        let (client, _) = try await makeConnected()
        do {
            try await client.setTime(epochMs: 1_704_067_199_999, utcOffsetMin: 0)
            XCTFail("epoch 低于窗口应本地拒绝")
        } catch BuzzLinkError.invalidParameters {
            // 预期
        }
        do {
            try await client.setTime(epochMs: 4_102_444_800_000, utcOffsetMin: 0)
            XCTFail("epoch 高于窗口应本地拒绝")
        } catch BuzzLinkError.invalidParameters {
            // 预期
        }
        await client.disconnect()
    }

    func testTimeSetOffsetOutOfRangeRejectedLocally() async throws {
        let (client, _) = try await makeConnected()
        do {
            try await client.setTime(epochMs: 1_704_067_200_000, utcOffsetMin: 841)
            XCTFail("超 +14h 应本地拒绝")
        } catch BuzzLinkError.invalidParameters {
            // 预期
        }
        do {
            try await client.setTime(epochMs: 1_704_067_200_000, utcOffsetMin: -721)
            XCTFail("超 -12h 应本地拒绝")
        } catch BuzzLinkError.invalidParameters {
            // 预期
        }
        await client.disconnect()
    }
}
