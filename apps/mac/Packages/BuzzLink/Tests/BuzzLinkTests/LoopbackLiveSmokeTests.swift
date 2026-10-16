import BuzzLink
import BuzzLinkCore
import BuzzLinkTestSupport
import Foundation
import Network
import XCTest

/// Loopback 活体冒烟（TESTING.md §4）：对运行在 127.0.0.1:40404 的真实 BuzzLinkSim
/// app 跑完整会话（连接 → INFO 推送 → INFO_GET → LIST → 全量下载 → CRC 验收）。
///
/// 手动跑法：先在 Xcode 运行 BuzzLinkSim app（loopback 默认开启），再执行
/// `swift test --filter LoopbackLiveSmokeTests`。Sim 未运行时测试自动 skip。
final class LoopbackLiveSmokeTests: XCTestCase {

    /// 探测 loopback 端口是否可达（0.75 s 超时）
    private static func isLoopbackReachable() async -> Bool {
        await withCheckedContinuation { continuation in
            let connection = NWConnection(
                host: "127.0.0.1",
                port: .init(rawValue: BuzzLinkConstants.loopbackPort) ?? 40404,
                using: .tcp
            )
            let stateContinuation = continuation
            connection.stateUpdateHandler = { state in
                switch state {
                case .ready:
                    // 先摘回调再 cancel/resume，避免兜底超时二次 resume
                    connection.stateUpdateHandler = nil
                    connection.cancel()
                    stateContinuation.resume(returning: true)
                case .failed, .cancelled:
                    connection.stateUpdateHandler = nil
                    stateContinuation.resume(returning: false)
                default:
                    break
                }
            }
            connection.start(queue: .global())
            DispatchQueue.global().asyncAfter(deadline: .now() + 0.75) {
                connection.cancel()
            }
        }
    }

    func testLiveSessionAgainstBuzzLinkSim() async throws {
        guard await Self.isLoopbackReachable() else {
            throw XCTSkip(
                "BuzzLinkSim 未运行：先在 Xcode 启动 BuzzLinkSim app（loopback 127.0.0.1:40404 默认开启）"
            )
        }

        let client = BuzzLinkClient(transport: LoopbackTransport())
        try await client.connect()
        defer { Task { await client.disconnect() } }

        // 连接建立后 Sim 主动推 INFO（§6.1，session=0x00）；有界轮询替代固定 sleep
        var pushed: BuzzLinkDeviceInfo?
        for _ in 0..<200 where pushed == nil {
            pushed = await client.lastDeviceInfo
            if pushed == nil {
                try await Task.sleep(for: .milliseconds(10))
            }
        }
        XCTAssertEqual(pushed?.model, "beecord-sim", "应收到 Sim 的 INFO 推送")

        // INFO_GET 与推送同值
        let info = try await client.getDeviceInfo()
        XCTAssertEqual(info.deviceName, "BCD-DEMO")
        XCTAssertEqual(info.protocolVersion, 1)

        // 列表 = Sim 内置 3 个合成样本
        let page = try await client.listFiles()
        XCTAssertEqual(page.total, 3)
        XCTAssertEqual(page.files.count, 3)

        // 全量下载并 CRC 验收
        for entry in page.files {
            let collector = ClientProgressRecorder()
            let result = try await client.download(fileID: entry.id) { collector.record($0) }
            XCTAssertEqual(result.size, entry.size, "\(entry.id) 大小须与列表一致")
            XCTAssertEqual(result.crc16, CRC16.ccittFalse(result.data), "\(entry.id) CRC 自洽")
            XCTAssertEqual(collector.values.last?.receivedBytes, entry.size)
        }

        await client.disconnect()
    }
}
