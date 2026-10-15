import BuzzLink
import BuzzLinkCore
import BuzzLinkSimCore
import XCTest

/// 进程内集成测试：真实 BuzzLinkClient 字节流经 SimEngineTransport 直接 pipe 进
/// SimEngine（TESTING.md §4 的进程内等价形态）——列文件、查信息、下载（含真实
/// ogg 样本）、CRC 验收，以及 SimEngine 故障注入下的错误/重试路径。
final class SimEngineIntegrationTests: XCTestCase {

    /// 样本文件（与 Sim app 内置样本同源：apps/mac/BuzzLinkSim/Resources/Samples/）
    private struct Sample {
        let id: String
        let durationMs: UInt32
        let data: Data
    }

    private func loadSamples() throws -> [Sample] {
        let samplesDir = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // BuzzLinkTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // BuzzLink（包根）
            .deletingLastPathComponent()   // Packages
            .deletingLastPathComponent()   // mac
            .appendingPathComponent("BuzzLinkSim/Resources/Samples")
        let specs = [
            ("sample-05s", "REC_20260101_120000", UInt32(5_006)),
            ("sample-30s", "REC_20260102_093000", UInt32(30_006)),
            ("sample-120s", "REC_20260103_074500", UInt32(120_006))
        ]
        return try specs.map { name, id, durationMs in
            let url = samplesDir.appendingPathComponent("\(name).ogg")
            let data = try Data(contentsOf: url)
            return Sample(id: id, durationMs: durationMs, data: data)
        }
    }

    private func makeFile(id: String, durationMs: UInt32, size: Int, seed: UInt64) -> SimFile {
        var rng = SplitMix64(seed: seed)
        return SimFile(
            id: id,
            durationMs: durationMs,
            data: Data((0..<size).map { _ in UInt8(truncatingIfNeeded: rng.next()) })
        )
    }

    private func makeClient(
        engine: SimEngine,
        commandTimeout: TimeInterval = 2,
        chunkTimeout: TimeInterval = 2,
        throttleBytesPerSecond: Int? = nil
    ) async throws -> (BuzzLinkClient, SimEngineTransport) {
        let transport = SimEngineTransport(
            engine: engine,
            throttleBytesPerSecond: throttleBytesPerSecond
        )
        let client = BuzzLinkClient(
            transport: transport,
            commandTimeout: commandTimeout,
            chunkTimeout: chunkTimeout
        )
        try await client.connect()
        return (client, transport)
    }

    // MARK: - INFO / LIST

    func testInfoGetReturnsProfile() async throws {
        let engine = SimEngine(files: [])
        let (client, _) = try await makeClient(engine: engine)
        let info = try await client.getDeviceInfo()
        XCTAssertEqual(info.model, "beecord-sim")
        XCTAssertEqual(info.deviceName, "BCD-DEMO")
        XCTAssertEqual(info.battery, 87)
        XCTAssertEqual(info.protocolVersion, 1)
        await client.disconnect()
    }

    func testInfoPushArrivesOnConnect() async throws {
        let engine = SimEngine(files: [])
        let (client, _) = try await makeClient(engine: engine)
        var iterator = client.deviceInfoUpdates.makeAsyncIterator()
        let pushed = await iterator.next()
        XCTAssertEqual(pushed?.deviceName, "BCD-DEMO")
        let last = await client.lastDeviceInfo
        XCTAssertEqual(last?.protocolVersion, 1)
        await client.disconnect()
    }

    func testListFilesNewestFirstAndPaginate() async throws {
        let samples = try loadSamples()
        let engine = SimEngine(files: samples.map {
            SimFile(id: $0.id, durationMs: $0.durationMs, data: $0.data)
        })
        let (client, _) = try await makeClient(engine: engine)

        let page = try await client.listFiles(offset: 0)
        XCTAssertEqual(page.total, 3)
        XCTAssertEqual(page.files.map(\.id), [
            "REC_20260103_074500",
            "REC_20260102_093000",
            "REC_20260101_120000"
        ])
        // 翻页：offset=1 剩 2 条
        let page2 = try await client.listFiles(offset: 1)
        XCTAssertEqual(page2.offset, 1)
        XCTAssertEqual(page2.files.count, 2)

        // FILE_INFO 与列表一致
        let info = try await client.fileInfo(id: samples[0].id)
        XCTAssertEqual(info.size, samples[0].data.count)
        XCTAssertEqual(info.crc16, CRC16.ccittFalse(samples[0].data))
        await client.disconnect()
    }

    func testFileInfoNotFound() async throws {
        let engine = SimEngine(files: [makeFile(id: "REC_20260101_120000", durationMs: 1_000, size: 100, seed: 1)])
        let (client, _) = try await makeClient(engine: engine)
        do {
            _ = try await client.fileInfo(id: "REC_29990101_000000")
            XCTFail("应抛 notFound")
        } catch BuzzLinkError.notFound {
            // 预期
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        await client.disconnect()
    }

    func testHostileFileNamesListable() async throws {
        var faults = SimFaults()
        faults.hostileFileNames = true
        let engine = SimEngine(files: [makeFile(id: "REC_20260101_120000", durationMs: 1_000, size: 100, seed: 1)], faults: faults)
        let (client, _) = try await makeClient(engine: engine)
        let page = try await client.listFiles(offset: 0)
        XCTAssertEqual(page.total, 3)
        let ids = page.files.map(\.id)
        XCTAssertTrue(ids.contains("REC_2026/06:06*?"))
        for id in ids {
            XCTAssertLessThanOrEqual(id.utf8.count, BuzzLinkConstants.maxFileIDLength)
        }
        await client.disconnect()
    }

    // MARK: - 下载（真实 ogg 样本）

    func testDownloadAllBundledSamples() async throws {
        let samples = try loadSamples()
        let engine = SimEngine(files: samples.map {
            SimFile(id: $0.id, durationMs: $0.durationMs, data: $0.data)
        })
        let (client, _) = try await makeClient(engine: engine)

        for sample in samples {
            let collector = ClientProgressRecorder()
            let result = try await client.download(fileID: sample.id) { collector.record($0) }
            XCTAssertEqual(result.data, sample.data, "\(sample.id) 内容须逐字节一致")
            XCTAssertEqual(result.crc16, CRC16.ccittFalse(sample.data))
            XCTAssertEqual(result.size, sample.data.count)
            // 进度末帧 = 整文件大小，且单调递增
            let values = collector.values
            XCTAssertEqual(values.last?.receivedBytes, sample.data.count)
            XCTAssertEqual(values.map(\.receivedBytes), values.map(\.receivedBytes).sorted())
            XCTAssertTrue(values.allSatisfy { $0.totalBytes == sample.data.count && $0.attempt == 1 })
        }
        await client.disconnect()
    }

    // MARK: - 故障注入（TESTING.md §5）

    func testDropSomeChunksRecoversViaRetry() async throws {
        // 确定性种子搜索：第一趟丢块、第二趟干净（SplitMix64 可复现）
        let fileID = "REC_20260101_120000"
        let file = makeFile(id: fileID, durationMs: 1_000, size: 10 * 224, seed: 5)
        var faults = SimFaults()
        faults.dropChunkProbability = 0.25
        let seed = try findSeed(file: file, faults: faults, firstDropsSecondClean: true)

        let engine = SimEngine(files: [file], faults: faults, randomSeed: seed)
        let (client, _) = try await makeClient(engine: engine, chunkTimeout: 0.3)
        let result = try await client.download(fileID: fileID)
        XCTAssertEqual(result.data, file.data)
        await client.disconnect()
    }

    func testDropAllChunksExhaustsRetries() async throws {
        // p=1.0：确定性全丢 → 三趟块超时 → downloadRetryExhausted
        let fileID = "REC_20260101_120000"
        let file = makeFile(id: fileID, durationMs: 1_000, size: 10 * 224, seed: 6)
        var faults = SimFaults()
        faults.dropChunkProbability = 1.0
        let engine = SimEngine(files: [file], faults: faults)
        let (client, _) = try await makeClient(engine: engine, commandTimeout: 0.5, chunkTimeout: 0.1)

        do {
            _ = try await client.download(fileID: fileID)
            XCTFail("全丢块应最终失败")
        } catch BuzzLinkError.downloadRetryExhausted(let failedID, let attempts, let lastError) {
            XCTAssertEqual(failedID, fileID)
            XCTAssertEqual(attempts, 3)
            guard case BuzzLinkError.chunkTimeout = lastError else {
                return XCTFail("末次错误应为 chunkTimeout，实际 \(lastError)")
            }
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        await client.disconnect()
    }

    func testCorruptFinalChunkCRCExhaustsRetries() async throws {
        // 末帧 CRC 损坏 → 帧级丢弃 → 客户端永远收不齐 → 每趟块超时 → 重试耗尽
        let fileID = "REC_20260101_120000"
        let file = makeFile(id: fileID, durationMs: 1_000, size: 772, seed: 8)
        var faults = SimFaults()
        faults.corruptFinalChunkCRC = true
        let engine = SimEngine(files: [file], faults: faults)
        let (client, _) = try await makeClient(engine: engine, commandTimeout: 0.5, chunkTimeout: 0.1)

        do {
            _ = try await client.download(fileID: fileID)
            XCTFail("持续末帧 CRC 错误应最终失败")
        } catch BuzzLinkError.downloadRetryExhausted(_, let attempts, let lastError) {
            XCTAssertEqual(attempts, 3)
            guard case BuzzLinkError.chunkTimeout = lastError else {
                return XCTFail("末次错误应为 chunkTimeout，实际 \(lastError)")
            }
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        await client.disconnect()
    }

    func testSilentMidDownloadFailsThenCleanReconnectSucceeds() async throws {
        // 中途静默（模拟断连）：fault 对每个新推流都会在 30% 进度触发（SimFaults 语义），
        // 客户端 3 趟耗尽 → downloadRetryExhausted（对应矩阵"文件标未完成"）；
        // "重连"（新会话、故障关闭）后重新下载成功（对应"重连后可重新下载"）。
        let fileID = "REC_20260101_120000"
        let file = makeFile(id: fileID, durationMs: 1_000, size: 10 * 224, seed: 9)

        var faults = SimFaults()
        faults.silentMidDownload = true
        faults.silentDuration = 0.4
        let faultyEngine = SimEngine(files: [file], faults: faults)
        let (client, _) = try await makeClient(
            engine: faultyEngine, commandTimeout: 0.25, chunkTimeout: 0.2
        )
        do {
            _ = try await client.download(fileID: fileID)
            XCTFail("静默持续触发时下载应耗尽重试")
        } catch BuzzLinkError.downloadRetryExhausted(_, let attempts, let lastError) {
            XCTAssertEqual(attempts, 3)
            // 静默期客户端挂在"等命令响应"还是"等块"取决于调度，两种超时皆为
            // 合法的"静默导致失败"表现（CI 慢 runner 上尤甚）；语义核心是重试耗尽。
            switch lastError {
            case .chunkTimeout, .commandTimeout:
                break
            default:
                XCTFail("末次错误应为 chunkTimeout 或 commandTimeout，实际 \(lastError)")
            }
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        await client.disconnect()

        // 重连：干净设备（同文件库），下载成功
        let cleanEngine = SimEngine(files: [file])
        let (client2, _) = try await makeClient(engine: cleanEngine)
        let result = try await client2.download(fileID: fileID)
        XCTAssertEqual(result.data, file.data)
        await client2.disconnect()
    }

    func testThrottledDownloadProgressesMonotonically() async throws {
        let fileID = "REC_20260101_120000"
        let file = makeFile(id: fileID, durationMs: 1_000, size: 20 * 224, seed: 10)
        let engine = SimEngine(files: [file])
        let (client, _) = try await makeClient(
            engine: engine,
            throttleBytesPerSecond: 20 * 224 * 4 // ~4 趟文件/秒 → 本趟约 0.25s
        )
        let collector = ClientProgressRecorder()
        let result = try await client.download(fileID: fileID) { collector.record($0) }
        XCTAssertEqual(result.data, file.data)
        let values = collector.values
        XCTAssertGreaterThan(values.count, 1, "限速下应产生多个进度回调")
        XCTAssertEqual(values.map(\.receivedBytes), values.map(\.receivedBytes).sorted())
        await client.disconnect()
    }

    // MARK: - 种子搜索辅助

    /// 找种子：第一趟丢 ≥1 块、第二趟 0 块（引擎 RNG 跨趟连续，与传输层行为一致）
    private func findSeed(
        file: SimFile,
        faults: SimFaults,
        firstDropsSecondClean: Bool
    ) throws -> UInt64 {
        for seed in 0..<1_000 {
            var probe = SimEngine(files: [file], faults: faults, randomSeed: UInt64(seed))
            let begin = BuzzLinkFrame(
                type: BuzzLinkFrameType.dlBeginReq.rawValue,
                session: 1,
                channel: BuzzLinkChannel.file.rawValue,
                payload: idPayload(file.id)
            )
            _ = probe.handle(begin, now: 0)
            while probe.nextChunk(now: 0) != nil {}
            let firstDrops = probe.droppedChunkCount
            probe.reset()
            let begin2 = BuzzLinkFrame(
                type: BuzzLinkFrameType.dlBeginReq.rawValue,
                session: 2,
                channel: BuzzLinkChannel.file.rawValue,
                payload: idPayload(file.id)
            )
            _ = probe.handle(begin2, now: 0)
            while probe.nextChunk(now: 0) != nil {}
            let secondDrops = probe.droppedChunkCount - firstDrops
            if firstDropsSecondClean, firstDrops > 0, secondDrops == 0 {
                return UInt64(seed)
            }
        }
        throw XCTSkip("未找到满足条件的随机种子（概率极小，可调高丢块率）")
    }

    /// `[idLen u8][id…]`（§5.1）
    private func idPayload(_ id: String) -> Data {
        var payload = Data([UInt8(id.utf8.count)])
        payload.append(Data(id.utf8))
        return payload
    }
}

/// 进度记录器（跨并发回调线程安全）
final class ClientProgressRecorder: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [BuzzLinkDownloadProgress] = []

    var values: [BuzzLinkDownloadProgress] {
        lock.withLock { storage }
    }

    func record(_ progress: BuzzLinkDownloadProgress) {
        lock.withLock { storage.append(progress) }
    }
}
