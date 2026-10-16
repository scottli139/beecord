import BuzzLink
import BuzzLinkCore
import BuzzLinkSimCore
import BuzzLinkTestSupport
import XCTest

/// 下载取消语义测试（合作式即时取消）：收块循环每块、每趟出发前、BEGIN 成立开工前
/// 检查任务取消；命中即中断本趟并尽力 DL_ABORT，状态收敛回 `.ready`，抛 `CancellationError`。
/// 在飞的单个命令等待不被打断，由命令超时兜底（注入缩短超时，有界断言替代固定 sleep）。
final class ClientCancellationTests: XCTestCase {

    private static let fileID = "REC_20260101_120000"

    private func makeFile(size: Int, seed: UInt64) -> SimFile {
        var rng = SplitMix64(seed: seed)
        return SimFile(
            id: Self.fileID,
            durationMs: 1_000,
            data: Data((0..<size).map { _ in UInt8(truncatingIfNeeded: rng.next()) })
        )
    }

    /// 有界轮询直到条件成立或超时（时序纪律：禁固定 sleep）
    private func pollUntil(
        timeout: TimeInterval,
        _ predicate: @escaping () async -> Bool
    ) async -> Bool {
        let deadline = ProcessInfo.processInfo.systemUptime + timeout
        while ProcessInfo.processInfo.systemUptime < deadline {
            if await predicate() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return await predicate()
    }

    // MARK: - 1. 收块循环中取消：即时中断、状态修复、设备侧无残留

    func testCancelMidDownloadThrowsPromptlyAndRecovers() async throws {
        let file = makeFile(size: 100 * BuzzLinkConstants.defaultChunkDataLength, seed: 21)
        let engine = SimEngine(files: [file])
        // 限速让 100 块跨越约 1s，留出取消窗口
        let transport = SimEngineTransport(engine: engine, throttleBytesPerSecond: 32_000)
        let client = BuzzLinkClient(transport: transport, commandTimeout: 2, chunkTimeout: 2)
        try await client.connect()

        let recorder = ClientProgressRecorder()
        let downloadTask = Task {
            try await client.download(fileID: file.id) { recorder.record($0) }
        }
        let progressed = await pollUntil(timeout: 2) { !recorder.values.isEmpty }
        XCTAssertTrue(progressed, "限速下载应在 2s 内产生首个进度回调")

        let cancelAt = ProcessInfo.processInfo.systemUptime
        downloadTask.cancel()
        let result = await withTaskResult(downloadTask)
        let elapsed = ProcessInfo.processInfo.systemUptime - cancelAt

        guard case .failure(let error) = result else {
            XCTFail("取消后下载不应成功完成")
            await client.disconnect()
            return
        }
        XCTAssertTrue(error is CancellationError, "应抛 CancellationError，实际 \(error)")
        XCTAssertLessThan(elapsed, 2.0, "合作式取消应即时生效（而非等块超时兜底），实际 \(elapsed)s")

        // 状态收敛回 .ready（clearDownload 的 defer 先于抛出执行；有界轮询防御时序）
        let ready = await pollUntil(timeout: 2) { await client.state == .ready }
        XCTAssertTrue(ready, "取消后状态应收敛回 .ready")

        // DL_ABORT 已清理设备侧推流：同会话重下成功（未清理则 BEGIN 会吃 busy）
        let retry = try await client.download(fileID: file.id)
        XCTAssertEqual(retry.data, file.data)
        await client.disconnect()
    }

    // MARK: - 2. BEGIN 在飞期间取消：命令超时兜底回收，取消路径补发 DL_ABORT

    func testCancelDuringBeginPhase() async throws {
        // 永不应答的挂起链路（MockTransport 不回包），命令超时注入 0.3s
        let transport = MockTransport()
        let client = BuzzLinkClient(transport: transport, commandTimeout: 0.3, chunkTimeout: 0.2)
        try await client.connect()

        // 局部绑定静态常量：@Sendable Task 闭包不捕获 XCTestCase 元类型（Swift 6 区域隔离）
        let fileID = Self.fileID
        let downloadTask = Task {
            try await client.download(fileID: fileID)
        }
        let beginSent = await pollUntil(timeout: 2) {
            transport.sentFrames.contains { $0.type == BuzzLinkFrameType.dlBeginReq.rawValue }
        }
        XCTAssertTrue(beginSent, "BEGIN 应已发出")

        let cancelAt = ProcessInfo.processInfo.systemUptime
        downloadTask.cancel()
        let result = await withTaskResult(downloadTask)
        let elapsed = ProcessInfo.processInfo.systemUptime - cancelAt

        guard case .failure(let error) = result else {
            XCTFail("取消后下载不应成功")
            await client.disconnect()
            return
        }
        XCTAssertTrue(error is CancellationError, "应抛 CancellationError，实际 \(error)")
        XCTAssertLessThan(elapsed, 1.0, "在飞 BEGIN 由命令超时（0.3s 注入）兜底，取消应在 1s 内收敛，实际 \(elapsed)s")

        let frames = transport.sentFrames
        XCTAssertTrue(
            frames.contains { $0.type == BuzzLinkFrameType.dlAbort.rawValue },
            "取消路径应尽力补发 DL_ABORT"
        )
        // BEGIN 从未成立，状态全程未离开 .ready
        let ready = await pollUntil(timeout: 2) { await client.state == .ready }
        XCTAssertTrue(ready)
        await client.disconnect()
    }

    // MARK: - 3. 趟间取消：首趟失败后不再发起第二趟

    func testCancelBetweenRetries() async throws {
        // 首趟 BEGIN 正常应答但块永不到达（注入块超时 0.3s 判负）；
        // 取消发生在第一趟收块等待中，趟间检查点须阻止第二趟发起。
        let box = WeakBox<MockTransport>()
        let transport = MockTransport(onSend: { bytes in
            guard let frame = try? BuzzLinkFrame.decoded(from: bytes),
                  frame.type == BuzzLinkFrameType.dlBeginReq.rawValue,
                  let response = try? TestResponseBytes.dlBeginResp(
                      session: frame.session,
                      totalChunks: 10,
                      size: UInt32(10 * BuzzLinkConstants.defaultChunkDataLength),
                      fileCRC16: 0
                  ) else { return }
            box.value?.emitInbound(response)
        })
        box.value = transport
        let client = BuzzLinkClient(transport: transport, commandTimeout: 0.5, chunkTimeout: 0.3)
        try await client.connect()

        // 局部绑定静态常量：@Sendable Task 闭包不捕获 XCTestCase 元类型（Swift 6 区域隔离）
        let fileID = Self.fileID
        let downloadTask = Task {
            try await client.download(fileID: fileID)
        }
        // BEGIN_RESP 成立后客户端进入 .downloading：此时取消（第一趟收块等待中）
        let downloading = await pollUntil(timeout: 2) {
            await client.state == .downloading(fileID: fileID)
        }
        XCTAssertTrue(downloading, "BEGIN_RESP 后应进入 downloading 态")
        downloadTask.cancel()
        let result = await withTaskResult(downloadTask)

        guard case .failure(let error) = result else {
            XCTFail("取消后下载不应成功")
            await client.disconnect()
            return
        }
        XCTAssertTrue(error is CancellationError, "趟间取消应抛 CancellationError（而非重试耗尽），实际 \(error)")

        // 结构性断言：只发起过一趟 BEGIN（取消阻止了第二趟），且至少一次 DL_ABORT
        let frames = transport.sentFrames
        let beginCount = frames.filter { $0.type == BuzzLinkFrameType.dlBeginReq.rawValue }.count
        XCTAssertEqual(beginCount, 1, "取消后不应发起第二趟下载")
        XCTAssertTrue(
            frames.contains { $0.type == BuzzLinkFrameType.dlAbort.rawValue },
            "失败趟与取消路径均应尽力补发 DL_ABORT"
        )

        let ready = await pollUntil(timeout: 2) { await client.state == .ready }
        XCTAssertTrue(ready)
        await client.disconnect()
    }
}
