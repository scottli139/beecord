import BuzzLink
import BuzzLinkCore
import BuzzLinkSimCore
import XCTest

/// 下载会话测试（BUZZLINK.md §6.2/§6.3/§7）：BEGIN 声明为准、三处 CRC 裁决、
/// 超时/丢块重试、busy/not_found 映射、断连失败。
final class ClientDownloadTests: XCTestCase {

    // MARK: - 测试辅助

    /// 进度收集器（回调经 @Sendable 闭包进入）
    final class ProgressCollector: @unchecked Sendable {
        private let lock = NSLock()
        private var storage: [BuzzLinkDownloadProgress] = []

        var values: [BuzzLinkDownloadProgress] {
            lock.withLock { storage }
        }

        func record(_ progress: BuzzLinkDownloadProgress) {
            lock.withLock { storage.append(progress) }
        }
    }

    /// 脚本化下载设备：按 BUZZLINK.md §5.1/§6.2 应答 BEGIN/END/ABORT 并推块；
    /// 计数器与故障开关供测试断言与编排。
    final class DownloadDevice: @unchecked Sendable {
        let fileID: String
        let data: Data
        var chunkDataLen = 224

        private(set) var beginCount = 0
        private(set) var endCount = 0
        private(set) var abortCount = 0
        private(set) var lastBeginSession: UInt8 = 0

        var beginStatus: BuzzLinkStatus = .ok
        var endStatus: BuzzLinkStatus = .ok
        /// 故障注入：覆盖 BEGIN 携带的 CRC（默认真值）
        var beginCRCOverride: UInt16?
        /// 故障注入：覆盖 BEGIN 声明的 size/totalChunks（需配对设置）
        var beginSizeOverride: Int?
        var beginTotalChunksOverride: Int?
        var dropSeqs: Set<Int> = []
        var duplicateSeqs: Set<Int> = []
        /// false = 收 BEGIN 后不自动推块（配合手动交付/超时测试）
        var autoDeliverChunks = true
        /// 推流进行中（ok BEGIN 置位、END/ABORT 清除）：期间再收 BEGIN 回 busy（§5.1）
        private(set) var isStreaming = false

        var crc: UInt16 { CRC16.ccittFalse(data) }

        init(fileID: String, data: Data) {
            self.fileID = fileID
            self.data = data
        }

        func handle(_ frame: BuzzLinkFrame, transport: MockTransport) {
            switch frame.type {
            case BuzzLinkFrameType.dlBeginReq.rawValue:
                beginCount += 1
                lastBeginSession = frame.session
                guard beginStatus == .ok else {
                    respond(
                        try? TestResponseBytes.dlBeginResp(
                            session: frame.session, status: beginStatus,
                            totalChunks: 0, size: 0, fileCRC16: 0
                        ),
                        transport: transport
                    )
                    return
                }
                guard !isStreaming else {
                    respond(
                        try? TestResponseBytes.dlBeginResp(
                            session: frame.session, status: .busy,
                            totalChunks: 0, size: 0, fileCRC16: 0
                        ),
                        transport: transport
                    )
                    return
                }
                isStreaming = true
                let size = beginSizeOverride ?? data.count
                let total = beginTotalChunksOverride ?? (size + chunkDataLen - 1) / chunkDataLen
                respond(
                    try? TestResponseBytes.dlBeginResp(
                        session: frame.session,
                        chunkDataLen: UInt16(chunkDataLen),
                        totalChunks: UInt32(total),
                        size: UInt32(size),
                        fileCRC16: beginCRCOverride ?? crc
                    ),
                    transport: transport
                )
                if autoDeliverChunks {
                    deliverChunks(session: frame.session, transport: transport)
                }
            case BuzzLinkFrameType.dlEndReq.rawValue:
                endCount += 1
                isStreaming = false
                respond(
                    try? TestResponseBytes.dlEndResp(session: frame.session, status: endStatus, fileCRC16: crc),
                    transport: transport
                )
            case BuzzLinkFrameType.dlAbort.rawValue:
                abortCount += 1
                isStreaming = false
                respond(
                    try? TestResponseBytes.dlEndResp(session: frame.session, status: .aborted, fileCRC16: crc),
                    transport: transport
                )
            default:
                break
            }
        }

        /// 推块（可配合 dropSeqs/duplicateSeqs 与手动调用）
        func deliverChunks(session: UInt8, transport: MockTransport) {
            let total = (data.count + chunkDataLen - 1) / chunkDataLen
            for seq in 0..<total {
                guard !dropSeqs.contains(seq) else { continue }
                let start = seq * chunkDataLen
                let end = min(start + chunkDataLen, data.count)
                guard let chunk = try? TestResponseBytes.dlChunk(
                    session: session, seq: UInt16(seq), data: data.subdata(in: start..<end)
                ) else { continue }
                transport.emitInbound(chunk)
                if duplicateSeqs.contains(seq) {
                    transport.emitInbound(chunk)
                }
            }
        }

        private func respond(_ bytes: Data?, transport: MockTransport) {
            if let bytes { transport.emitInbound(bytes) }
        }
    }

    private func makeFileData(size: Int, seed: UInt64 = 7) -> Data {
        var rng = SplitMix64(seed: seed)
        return Data((0..<size).map { _ in UInt8(truncatingIfNeeded: rng.next()) })
    }

    /// 连接好的客户端 + 设备脚本
    private func makeConnected(
        device: DownloadDevice,
        commandTimeout: TimeInterval = 1,
        chunkTimeout: TimeInterval = 1,
        extraScript: (@Sendable (BuzzLinkFrame, MockTransport) -> Void)? = nil
    ) async throws -> (BuzzLinkClient, MockTransport) {
        let box = WeakBox<MockTransport>()
        let transport = MockTransport { bytes in
            guard let transport = box.value,
                  let frame = try? BuzzLinkFrame.decoded(from: bytes) else { return }
            device.handle(frame, transport: transport)
            extraScript?(frame, transport)
        }
        box.value = transport
        let client = BuzzLinkClient(
            transport: transport,
            commandTimeout: commandTimeout,
            chunkTimeout: chunkTimeout
        )
        try await client.connect()
        return (client, transport)
    }

    // MARK: - 完整下载

    func testFullDownloadVerifiesThreeWayCRC() async throws {
        let fileID = "REC_20260101_120000"
        let data = makeFileData(size: 1_000, seed: 11)
        let device = DownloadDevice(fileID: fileID, data: data)
        let collector = ProgressCollector()
        let (client, _) = try await makeConnected(device: device)

        let states = ClientStateCollector()
        let collecting = Task {
            for await state in client.stateUpdates {
                states.append(state)
            }
        }

        let result = try await client.download(fileID: fileID) { collector.record($0) }
        XCTAssertEqual(result.data, data)
        XCTAssertEqual(result.size, data.count)
        XCTAssertEqual(result.crc16, CRC16.ccittFalse(data))

        // 进度：receivedBytes 单调递增至 total（attempt 恒 1）
        let progress = collector.values
        XCTAssertEqual(progress.count, 5, "1000 字节 / 224 = 5 块")
        XCTAssertTrue(progress.allSatisfy { $0.attempt == 1 && $0.totalBytes == data.count })
        XCTAssertEqual(progress.map(\.receivedBytes), [224, 448, 672, 896, 1_000])

        XCTAssertEqual(device.beginCount, 1)
        XCTAssertEqual(device.endCount, 1)
        XCTAssertEqual(device.abortCount, 0)

        // 状态流经观察循环异步投递：await download 返回只保证 actor 侧已 yield 末态，
        // 不保证消费任务已 append；有界轮询等记录器收敛到终态 .ready 再断言全序列
        // （CI 慢 runner 上消费任务滞后导致序列缺尾，run 38854220197 抖动根因）
        let converged = await pollUntil(timeout: 5) { states.values.last == .ready }
        XCTAssertTrue(converged, "状态记录应收敛到终态 .ready")
        collecting.cancel()
        XCTAssertEqual(
            states.values,
            [.connecting, .ready, .downloading(fileID: fileID), .verifying(fileID: fileID), .ready]
        )
        await client.disconnect()
    }

    func testDownloadRespectsDeclaredChunkDataLen() async throws {
        // 设备声明非默认块长 200：除末块外每块恰 200（§6.2）
        let fileID = "REC_20260102_120000"
        let data = makeFileData(size: 500, seed: 12)
        let device = DownloadDevice(fileID: fileID, data: data)
        device.chunkDataLen = 200
        let collector = ProgressCollector()
        let (client, _) = try await makeConnected(device: device)

        let result = try await client.download(fileID: fileID) { collector.record($0) }
        XCTAssertEqual(result.data, data)
        XCTAssertEqual(collector.values.map(\.receivedBytes), [200, 400, 500])
        await client.disconnect()
    }

    func testOutOfOrderAndDuplicateChunksAccepted() async throws {
        // §7：乱序重组、重复忽略，CRC 终裁
        let fileID = "REC_20260103_120000"
        let data = makeFileData(size: 672, seed: 13) // 3 块
        let device = DownloadDevice(fileID: fileID, data: data)
        device.autoDeliverChunks = false
        let (client, transport) = try await makeConnected(device: device)

        let downloadTask = Task {
            try await client.download(fileID: fileID)
        }
        while device.beginCount < 1 {
            try await Task.sleep(for: .milliseconds(1))
        }
        let session = device.lastBeginSession
        // 逆序 + 重复 + 乱序混发
        for seq in [2, 0, 2, 1, 0] {
            let start = seq * 224
            let end = min(start + 224, data.count)
            let chunk = try TestResponseBytes.dlChunk(
                session: session, seq: UInt16(seq), data: data.subdata(in: start..<end)
            )
            transport.emitInbound(chunk)
        }
        let result = try await downloadTask.value
        XCTAssertEqual(result.data, data)
        await client.disconnect()
    }

    // MARK: - 设备错误映射（不重试）

    func testBeginNotFoundThrowsWithoutRetry() async throws {
        let device = DownloadDevice(fileID: "REC_X", data: makeFileData(size: 100))
        device.beginStatus = .notFound
        let (client, _) = try await makeConnected(device: device)
        do {
            _ = try await client.download(fileID: "REC_X")
            XCTFail("应抛 notFound")
        } catch BuzzLinkError.notFound {
            // 预期
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        XCTAssertEqual(device.beginCount, 1, "not_found 不可重试")
        let state = await client.state
        XCTAssertEqual(state, .ready, "失败趟结束状态回 ready")
        await client.disconnect()
    }

    func testBeginBusyWhenSecondDownloadStarts() async throws {
        // 并发第二个下载：客户端状态守卫（invalidState）或设备 busy 均为合法失败路径；
        // 取决于第二个调用是否赶在第一个 BEGIN 响应前进入。
        let fileID = "REC_20260101_120000"
        let data = makeFileData(size: 448, seed: 14)
        let device = DownloadDevice(fileID: fileID, data: data)
        device.autoDeliverChunks = false
        let (client, transport) = try await makeConnected(device: device)

        let first = Task {
            try await client.download(fileID: fileID)
        }
        while device.beginCount < 1 {
            try await Task.sleep(for: .milliseconds(1))
        }
        // 快照第一趟的 BEGIN session（第二个 BEGIN 会改写 lastBeginSession）
        let firstSession = device.lastBeginSession
        // 给第二个下载一个进入窗口（两种结局都在毫秒级落定）
        let second = Task {
            try await client.download(fileID: fileID)
        }
        let secondResult = await withTaskResult(second)

        // 结局一：客户端状态守卫拒绝（未发 BEGIN，beginCount 恒 1）
        // 结局二：BEGIN 已发、设备回 busy
        switch secondResult {
        case .success:
            XCTFail("并发第二个下载不应成功")
        case .failure(let error):
            guard case BuzzLinkError.busy = error else {
                guard case BuzzLinkError.invalidState = error else {
                    return XCTFail("第二个下载应以 busy/invalidState 失败，实际 \(error)")
                }
                break
            }
            XCTAssertEqual(device.beginCount, 2)
        }

        // 第一趟正常完成（用快照的 session：第二个 BEGIN 可能已改写 lastBeginSession）
        device.deliverChunks(session: firstSession, transport: transport)
        let result = try await first.value
        XCTAssertEqual(result.data, data)
        await client.disconnect()
    }

    func testDownloadRejectsOversizedFileID() async throws {
        let device = DownloadDevice(fileID: "x", data: Data())
        let (client, transport) = try await makeConnected(device: device)
        let longID = String(repeating: "A", count: 33)
        do {
            _ = try await client.download(fileID: longID)
            XCTFail("超长文件 ID 应抛 invalidParameters")
        } catch BuzzLinkError.invalidParameters {
            // 预期
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        XCTAssertTrue(transport.sentFrames.isEmpty)
        await client.disconnect()
    }

    // MARK: - 重试路径（§7）

    func testCRCMismatchRetriesThenSucceeds() async throws {
        let fileID = "REC_20260101_120000"
        let data = makeFileData(size: 300, seed: 15)
        let device = DownloadDevice(fileID: fileID, data: data)
        let collector = ProgressCollector()
        let box = WeakBox<MockTransport>()
        let transport = MockTransport { bytes in
            guard let transport = box.value,
                  let frame = try? BuzzLinkFrame.decoded(from: bytes) else { return }
            // 第一趟 BEGIN 携带假 CRC → 客户端本地 CRC 裁决失败 → 重试
            device.beginCRCOverride = device.beginCount == 0 ? device.crc ^ 0xFFFF : nil
            device.handle(frame, transport: transport)
        }
        box.value = transport
        let client = BuzzLinkClient(transport: transport, commandTimeout: 1, chunkTimeout: 1)
        try await client.connect()

        let result = try await client.download(fileID: fileID) { collector.record($0) }
        XCTAssertEqual(result.data, data)
        XCTAssertEqual(device.beginCount, 2)
        XCTAssertEqual(device.abortCount, 1, "重试前应 DL_ABORT 清理设备侧")
        // 进度 attempt 编号随趟数递增
        let attempts = Set(collector.values.map(\.attempt))
        XCTAssertEqual(attempts, [1, 2])
        await client.disconnect()
    }

    func testPersistentCRCMismatchExhaustsRetries() async throws {
        let fileID = "REC_20260101_120000"
        let data = makeFileData(size: 300, seed: 16)
        let device = DownloadDevice(fileID: fileID, data: data)
        device.beginCRCOverride = CRC16.ccittFalse(data) ^ 0xFFFF
        let (client, _) = try await makeConnected(device: device)
        do {
            _ = try await client.download(fileID: fileID)
            XCTFail("持续 CRC 错误应最终失败")
        } catch BuzzLinkError.downloadRetryExhausted(let failedID, let attempts, let lastError) {
            XCTAssertEqual(failedID, fileID)
            XCTAssertEqual(attempts, 3, "默认 = 首趟 + §7 允许 2 次重试")
            guard case BuzzLinkError.crcMismatch = lastError else {
                return XCTFail("末次错误应为 crcMismatch，实际 \(lastError)")
            }
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        XCTAssertEqual(device.beginCount, 3)
        XCTAssertEqual(device.abortCount, 2)
        let state = await client.state
        XCTAssertEqual(state, .ready)
        await client.disconnect()
    }

    func testChunkStallTimesOutAndRetries() async throws {
        let fileID = "REC_20260101_120000"
        let data = makeFileData(size: 448, seed: 17)
        let device = DownloadDevice(fileID: fileID, data: data)
        device.autoDeliverChunks = false
        let box = WeakBox<MockTransport>()
        let transport = MockTransport { bytes in
            guard let transport = box.value,
                  let frame = try? BuzzLinkFrame.decoded(from: bytes) else { return }
            if frame.type == BuzzLinkFrameType.dlAbort.rawValue {
                // ABORT 后放行后续 BEGIN 的推块
                device.autoDeliverChunks = true
            }
            device.handle(frame, transport: transport)
        }
        box.value = transport
        let client = BuzzLinkClient(transport: transport, commandTimeout: 1, chunkTimeout: 0.05)
        try await client.connect()

        let result = try await client.download(fileID: fileID)
        XCTAssertEqual(result.data, data)
        XCTAssertEqual(device.beginCount, 2, "首趟块超时后重试一趟")
        XCTAssertEqual(device.abortCount, 1)
        await client.disconnect()
    }

    func testMissingChunkCausesCRCFailureThenRetrySucceeds() async throws {
        let fileID = "REC_20260101_120000"
        let data = makeFileData(size: 672, seed: 18)
        let device = DownloadDevice(fileID: fileID, data: data)
        device.dropSeqs = [1]
        let box = WeakBox<MockTransport>()
        let transport = MockTransport { bytes in
            guard let transport = box.value,
                  let frame = try? BuzzLinkFrame.decoded(from: bytes) else { return }
            if frame.type == BuzzLinkFrameType.dlAbort.rawValue {
                device.dropSeqs = []
            }
            device.handle(frame, transport: transport)
        }
        box.value = transport
        let client = BuzzLinkClient(transport: transport, commandTimeout: 1, chunkTimeout: 0.05)
        try await client.connect()

        let result = try await client.download(fileID: fileID)
        XCTAssertEqual(result.data, data)
        XCTAssertEqual(device.beginCount, 2)
        await client.disconnect()
    }

    func testSizeMismatchRetriesThenSucceeds() async throws {
        let fileID = "REC_20260101_120000"
        let data = makeFileData(size: 448, seed: 19)
        let device = DownloadDevice(fileID: fileID, data: data)
        let box = WeakBox<MockTransport>()
        let transport = MockTransport { bytes in
            guard let transport = box.value,
                  let frame = try? BuzzLinkFrame.decoded(from: bytes) else { return }
            if device.beginCount == 0, frame.type == BuzzLinkFrameType.dlBeginReq.rawValue {
                // 首趟：声明 size 比实际少 48 字节；totalChunks 与声明 size 自洽（§6.2 校验）
                device.beginSizeOverride = data.count - 48
                device.beginTotalChunksOverride = (data.count - 48 + 223) / 224
            } else {
                device.beginSizeOverride = nil
                device.beginTotalChunksOverride = nil
            }
            device.handle(frame, transport: transport)
        }
        box.value = transport
        let client = BuzzLinkClient(transport: transport, commandTimeout: 1, chunkTimeout: 1)
        try await client.connect()

        let result = try await client.download(fileID: fileID)
        XCTAssertEqual(result.data, data)
        XCTAssertEqual(device.beginCount, 2)
        await client.disconnect()
    }

    func testDisconnectMidDownloadFailsDownload() async throws {
        let fileID = "REC_20260101_120000"
        let data = makeFileData(size: 448, seed: 20)
        let device = DownloadDevice(fileID: fileID, data: data)
        device.autoDeliverChunks = false
        let box = WeakBox<MockTransport>()
        let transport = MockTransport { bytes in
            guard let transport = box.value,
                  let frame = try? BuzzLinkFrame.decoded(from: bytes) else { return }
            device.handle(frame, transport: transport)
            if frame.type == BuzzLinkFrameType.dlBeginReq.rawValue {
                transport.failSends()
            }
        }
        box.value = transport
        let client = BuzzLinkClient(transport: transport, commandTimeout: 1, chunkTimeout: 1)
        try await client.connect()

        do {
            _ = try await client.download(fileID: fileID)
            XCTFail("断连后下载应失败")
        } catch BuzzLinkError.disconnected {
            // 预期
        } catch {
            XCTFail("意外错误类型：\(error)")
        }
        let state = await client.state
        XCTAssertEqual(state, .disconnected)
    }

    // MARK: - 下载并行性（§3.4）

    func testControlCommandRunsDuringDownload() async throws {
        let fileID = "REC_20260101_120000"
        let data = makeFileData(size: 448, seed: 21)
        let device = DownloadDevice(fileID: fileID, data: data)
        device.autoDeliverChunks = false
        let (client, transport) = try await makeConnected(device: device) { frame, transport in
            if frame.type == BuzzLinkFrameType.infoGet.rawValue,
               let info = try? TestResponseBytes.info(session: frame.session) {
                transport.emitInbound(info)
            }
        }

        let downloadTask = Task {
            try await client.download(fileID: fileID)
        }
        while device.beginCount < 1 {
            try await Task.sleep(for: .milliseconds(1))
        }
        // 推流进行中控制通道可并行（§3.4）
        let info = try await client.getDeviceInfo()
        XCTAssertEqual(info.deviceName, "BCD-TEST")

        device.deliverChunks(session: device.lastBeginSession, transport: transport)
        let result = try await downloadTask.value
        XCTAssertEqual(result.data, data)
        await client.disconnect()
    }
}
