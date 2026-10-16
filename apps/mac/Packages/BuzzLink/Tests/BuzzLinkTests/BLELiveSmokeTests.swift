import BuzzLink
import BuzzLinkCore
import BuzzLinkTestSupport
import Foundation
import XCTest
import os

/// BLE 跨机活体冒烟（TESTING.md §4）：Mac（Central，BLETransport）↔ 另一台设备前台运行的
/// BuzzLinkSim（BLE 外设广播）跑完整会话（扫描 → 连接 → INFO 推送 → INFO_GET → LIST →
/// 全量下载 → CRC 验收），并记录实测吞吐。
///
/// 手动跑法：
/// 1. 另一台设备（iPhone/iPad/另一台 Mac）前台运行 BuzzLinkSim，确认 BLE 广播已开启、
///    故障注入全部关闭（默认 3 个内置样本）；
/// 2. 本机蓝牙开启且已授权（首次会弹系统授权框）；
/// 3. `BLE_LIVE_SMOKE=1 swift test --filter BLELiveSmokeTests`。
/// 未设置环境变量时测试自动 skip（避免常规跑批/CI 误触发蓝牙扫描）。
final class BLELiveSmokeTests: XCTestCase {

    func testLiveSessionOverBLE() async throws {
        guard ProcessInfo.processInfo.environment["BLE_LIVE_SMOKE"] == "1" else {
            throw XCTSkip("未设置 BLE_LIVE_SMOKE=1（需对端设备前台运行 BuzzLinkSim 并开启 BLE 广播）")
        }

        let transport = await MainActor.run { BLETransport() }
        let client = BuzzLinkClient(transport: transport)
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

        let info = try await client.getDeviceInfo()
        XCTAssertEqual(info.protocolVersion, 1)

        // 列表 = Sim 内置 3 个合成样本（默认配置）
        let page = try await client.listFiles()
        XCTAssertEqual(page.total, 3, "Sim 应为默认 3 样本配置（勿开故障注入）")
        XCTAssertEqual(page.files.count, 3)

        // 全量下载并 CRC 验收，同时记录吞吐（T3 同链路实测参考值 ≈24 KB/s）
        let startedAt = ProcessInfo.processInfo.systemUptime
        var totalBytes = 0
        for entry in page.files {
            let collector = ClientProgressRecorder()
            let result = try await client.download(fileID: entry.id) { collector.record($0) }
            XCTAssertEqual(result.size, entry.size, "\(entry.id) 大小须与列表一致")
            XCTAssertEqual(result.crc16, CRC16.ccittFalse(result.data), "\(entry.id) CRC 自洽")
            XCTAssertEqual(collector.values.last?.receivedBytes, entry.size)
            totalBytes += result.size
        }
        let elapsed = ProcessInfo.processInfo.systemUptime - startedAt
        // 吞吐仅作记录不断言下限（链路环境差异大）；数字直接插值（统一日志中 public 可读）
        let kbps = Double(totalBytes) / max(elapsed, 0.001) / 1024
        Logger(subsystem: "com.beeuc.beecord.buzzlink", category: "ble-live").notice("BLE 实测吞吐：\(totalBytes) 字节，耗时 \(elapsed) 秒，约 \(kbps) KB/s")
        XCTAssertGreaterThan(totalBytes, 0)

        await client.disconnect()
    }
}
