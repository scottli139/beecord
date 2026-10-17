import BuzzLinkCore
import BuzzLinkSimCore
import XCTest

/// 设备流端到端 UI 测试（T5 阶段 D；TESTING.md §4：AX 树驱动）。
/// 设备源 = 测试进程内 UITestSimServer（loopback 40404）；每用例独立库根（-libraryRoot），
/// 端口被本机 Sim app 占用时 XCTSkip。全部判定走有界轮询，禁固定 sleep。
@MainActor
final class DeviceSyncUITests: XCTestCase {

    /// 与 Sim 内置样本一致的 3 个合成文件（确定性伪数据：协议层只关心长度与 CRC）
    private static let sampleIDs = ["REC_20260103_074500", "REC_20260102_093000", "REC_20260101_120000"]

    private var app: XCUIApplication!

    /// teardown 用共享态：tearDownWithError 为非隔离上下文，跨线程数据放 Sendable 盒
    nonisolated private let state = TestState()

    private final class TestState: @unchecked Sendable {
        var server: UITestSimServer?
        var libraryRoot: URL?
    }

    override func tearDownWithError() throws {
        state.server?.stop()
        state.server = nil
        // app 由 XCUITest 在用例结束后自动终止
        if let libraryRoot = state.libraryRoot {
            try? FileManager.default.removeItem(at: libraryRoot)
            state.libraryRoot = nil
        }
    }

    // MARK: - 基础设施

    /// 类型无关的 AX id 查询
    private func element(_ identifier: String) -> XCUIElement {
        app.descendants(matching: .any).matching(identifier: identifier).firstMatch
    }

    /// 有界等待元素满足谓词（XCTNSPredicateExpectation 求值：元素缺失/消失不抛快照错误）
    @discardableResult
    private func waitForElement(_ identifier: String, predicate: NSPredicate, timeout: TimeInterval = 30) -> Bool {
        let expectation = XCTNSPredicateExpectation(predicate: predicate, object: element(identifier))
        return XCTWaiter().wait(for: [expectation], timeout: timeout) == .completed
    }

    /// 行状态徽标文本等待（R4：状态为可读文本；SwiftUI StaticText 文本在 AXValue）
    @discardableResult
    private func waitForStatus(_ id: String, equal text: String, timeout: TimeInterval = 30) -> Bool {
        waitForElement(
            "device.fileTable.row.\(id).status",
            predicate: NSPredicate(format: "value == %@", text),
            timeout: timeout
        )
    }

    /// 行状态前缀等待（下载中 xx% 等动态文本）
    @discardableResult
    private func waitForStatusPrefix(_ id: String, _ prefix: String, timeout: TimeInterval = 30) -> Bool {
        waitForElement(
            "device.fileTable.row.\(id).status",
            predicate: NSPredicate(format: "value BEGINSWITH %@", prefix),
            timeout: timeout
        )
    }

    /// 元素出现有界轮询（0.2s 步进；短驻留 toast 用）
    private func pollElementExists(_ identifier: String, timeout: TimeInterval) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if element(identifier).exists { return true }
            Thread.sleep(forTimeInterval: 0.2)
        }
        return element(identifier).exists
    }

    /// 文件出现有界轮询（1s 步进）
    private func pollFileExists(_ url: URL, timeout: TimeInterval) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if FileManager.default.fileExists(atPath: url.path) { return true }
            Thread.sleep(forTimeInterval: 1)
        }
        return FileManager.default.fileExists(atPath: url.path)
    }

    /// 元素存在性等待
    @discardableResult
    private func waitForExistence(_ identifier: String, timeout: TimeInterval = 30) -> Bool {
        waitForElement(identifier, predicate: NSPredicate(format: "exists == true"), timeout: timeout)
    }

    /// 设备卡等待；失败时探测页面关键元素 + 启动探针写进失败信息（CI 无 xcresult 时的现场快照）
    private func requireDeviceCard(timeout: TimeInterval = 30, file: StaticString = #filePath, line: UInt = #line) {
        if waitForExistence("device.deviceCard", timeout: timeout) { return }
        let probes = [
            "device.emptyState", "device.scanButton", "device.bluetoothOverlay",
            "sidebar.navItem.devices", "sidebar.navItem.library", "common.toast"
        ]
        let present = probes.filter { element($0).exists }
        var message = "设备信息卡 \(Int(timeout))s 内未出现；在场元素：\(present.isEmpty ? "（均无）" : present.joined(separator: ", "))"
        if let trace = launchTraceTail() {
            message += "\n启动探针：\n\(trace)"
        }
        XCTFail(message, file: file, line: line)
    }

    /// 元素消失等待
    @discardableResult
    private func waitForDisappearance(_ identifier: String, timeout: TimeInterval = 30) -> Bool {
        waitForElement(identifier, predicate: NSPredicate(format: "exists == false"), timeout: timeout)
    }

    /// 启动被测 App：独立临时库根 + 重置设置（可选关自动同步）
    private func launchApp(autoSyncOff: Bool = false) throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("beecord-uitest-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        state.libraryRoot = root
        let app = XCUIApplication()
        var arguments = ["-resetSettings", "-libraryRoot", root.path, "-uiTestWindow"]
        if autoSyncOff {
            arguments.append("-autoSyncOff")
        }
        app.launchArguments = arguments
        app.launch()
        app.activate()
        self.app = app
        // 兜底：主窗口未随启动出现（WindowGroup 恢复态异常等）时，激活 + ⌘N 强制新建主窗口
        if !waitForExistence("sidebar.navItem.devices", timeout: 15) {
            app.activate()
            app.typeKey("n", modifierFlags: .command)
            _ = waitForExistence("sidebar.navItem.devices", timeout: 5)
        }
    }

    /// 启动探针尾段（app 侧 LaunchTrace 写入库根；失败信息现场的一部分）
    private func launchTraceTail() -> String? {
        guard let root = state.libraryRoot else { return nil }
        let url = root.appendingPathComponent("uitest-launch-trace.log")
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
        return String(text.suffix(600))
    }

    /// 启动进程内 Sim server；端口占用（本机 Sim app 在跑）→ 重试后 XCTSkip
    private func startServerOrSkip(faults: SimFaults = SimFaults()) throws {
        let server = UITestSimServer(engine: SimEngine(files: Self.makeSampleFiles(), faults: faults))
        for attempt in 1...3 {
            do {
                try server.start()
                self.state.server = server
                return
            } catch {
                if attempt < 3 {
                    RunLoop.current.run(until: Date().addingTimeInterval(0.5))
                } else {
                    throw XCTSkip("UITestSimServer 无法启动：\(error.localizedDescription)")
                }
            }
        }
    }

    /// 合成 ogg 样本：进程内生成真实 Ogg/Opus 静音流（WebRTC 静音帧 0xF8FFFE/20ms，
    /// 零文件依赖——UITest runner 有沙箱继承，读仓库 Fixtures 会被 TCC 拒绝）。
    private static func makeSampleFiles() -> [SimFile] {
        [
            SimFile(id: "REC_20260101_120000", durationMs: 5_000, data: SyntheticOgg.silence(durationSeconds: 5)),
            SimFile(id: "REC_20260102_093000", durationMs: 30_000, data: SyntheticOgg.silence(durationSeconds: 30)),
            SimFile(id: "REC_20260103_074500", durationMs: 120_000, data: SyntheticOgg.silence(durationSeconds: 120))
        ]
    }

    /// 库内某录音目录的入库产物是否齐备（audio.ogg + meta.json）
    private func libraryContainsRecording(_ id: String) -> Bool {
        guard let libraryRoot = state.libraryRoot else { return false }
        let dir = libraryRoot.appendingPathComponent("Library/\(id)")
        return FileManager.default.fileExists(atPath: dir.appendingPathComponent("audio.ogg").path)
            && FileManager.default.fileExists(atPath: dir.appendingPathComponent("meta.json").path)
    }

    /// Library 下的录音目录数（不含 .partial）
    private func libraryRecordingCount() -> Int {
        guard let libraryRoot = state.libraryRoot,
              let entries = try? FileManager.default.contentsOfDirectory(
                  at: libraryRoot.appendingPathComponent("Library"),
                  includingPropertiesForKeys: nil
              ) else {
            return 0
        }
        // 排除 .partial 与 catalog manifest（.catalog.json）等隐藏文件
        return entries.filter { !$0.lastPathComponent.hasPrefix(".") }.count
    }

    /// .partial 残留文件数
    private func partialFileCount() -> Int {
        guard let libraryRoot = state.libraryRoot,
              let entries = try? FileManager.default.contentsOfDirectory(
                  at: libraryRoot.appendingPathComponent("Library/.partial"),
                  includingPropertiesForKeys: nil
              ) else {
            return 0
        }
        return entries.count
    }

    // MARK: - 1. 连接：设备卡 + 文件列表

    func testConnectShowsDeviceCardAndFileList() throws {
        try startServerOrSkip()
        try launchApp(autoSyncOff: true)
        requireDeviceCard()
        XCTAssertTrue(waitForExistence("device.infoCard.battery", timeout: 10))
        XCTAssertTrue(waitForExistence("device.infoCard.storage", timeout: 10))
        for id in Self.sampleIDs {
            XCTAssertTrue(
                waitForExistence("device.fileTable.row.\(id)", timeout: 10),
                "文件行 \(id) 存在"
            )
            XCTAssertTrue(
                waitForStatus(id, equal: "未下载", timeout: 10),
                "行 \(id) 初始未下载（自动同步已关）"
            )
        }
    }

    // MARK: - 2. 勾选下载：行校验 + toast + 库产物

    func testDownloadSelectedVerifiesAndWritesLibrary() throws {
        try startServerOrSkip()
        try launchApp(autoSyncOff: true)
        let target = Self.sampleIDs[2]  // 最小文件
        requireDeviceCard()
        XCTAssertTrue(waitForExistence("device.fileTable.row.\(target).status", timeout: 10))

        element("device.fileTable.row.\(target).checkbox").click()
        let downloadButton = element("device.downloadButton")
        XCTAssertTrue(waitForExistence("device.downloadButton", timeout: 5))
        let title = downloadButton.label
        XCTAssertTrue(title.contains("下载所选") && title.contains("1"), "按钮标题含选中数，实际：\(title)")
        downloadButton.click()

        XCTAssertTrue(
            waitForStatus(target, equal: "✓ 已校验"),
            "行状态变为 ✓ 已校验"
        )
        // toast 驻留 2s：0.2s 步进轮询，避免 XCTWaiter 1s 轮询错过窗口
        XCTAssertTrue(pollElementExists("common.toast", timeout: 15), "同步完成 toast 出现")
        XCTAssertTrue(libraryContainsRecording(target), "库目录出现 audio.ogg + meta.json")
        XCTAssertEqual(libraryRecordingCount(), 1, "仅所选文件入库")
    }

    // MARK: - 3. 自动同步：全部校验 + 进度推进至终态后消失 + 库内 3 目录

    func testAutoSyncDownloadsAll() throws {
        // 中度限速：853KB 总量 @100KB/s ≈ 8.5s 进度可见窗口，50ms 轮询稳定采到 ≥0.9 峰值
        // （全速 loopback 下窗口仅数百毫秒，0.9 阈值本质是采样抖动；提前 break 避开
        // 元素消失瞬间的 .value 竞态——该竞态 XCUITest 记为失败且不可捕获）
        // 静音 ogg 总量仅 ~24KB：4KB/s 限速 ≈ 6s 进度窗口（100KB/s 会亚秒完成无窗口）
        var faults = SimFaults()
        faults.throttleBytesPerSecond = 4_096
        try startServerOrSkip(faults: faults)
        try launchApp()
        requireDeviceCard()
        // 进度条仅在同步期存在（完成即收敛 nil）：先确认出现并推进到近满，
        // 再确认其随完成消失（终态 1 的快照同 runloop 收敛，AX 不可稳定捕捉等值）
        // AXValue 实测为 NSNumber 0-1；XCTNSPredicateExpectation 对 value 谓词不可靠，
        // 故直接轮询 .value 取峰值（完成前 progress 即收敛 nil，1.0 渲染窗不可采样）
        let deadline = Date().addingTimeInterval(30)
        var peak = 0.0
        while Date() < deadline {
            let progress = element("device.downloadProgress")
            if progress.exists, let value = progress.value as? NSNumber {
                peak = max(peak, value.doubleValue)
                if peak >= 0.9 { break }
            }
            RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        }
        XCTAssertGreaterThanOrEqual(peak, 0.9, "整体进度推进 ≥ 90%（实际峰值 \(peak)）")
        XCTAssertTrue(
            waitForDisappearance("device.downloadProgress", timeout: 30),
            "完成后进度条收敛消失"
        )
        for id in Self.sampleIDs {
            XCTAssertTrue(
                waitForStatus(id, equal: "✓ 已校验"),
                "自动同步完成：行 \(id) 已校验"
            )
        }
        for id in Self.sampleIDs {
            XCTAssertTrue(libraryContainsRecording(id), "库内存在 \(id)")
        }
        XCTAssertEqual(libraryRecordingCount(), 3, "库内 3 个录音目录")
    }

    // MARK: - 4. 限速下载中取消：进度消失 + 行回未下载 + .partial 清理

    func testCancelDuringDownload() throws {
        var faults = SimFaults()
        faults.throttleBytesPerSecond = 4_096
        try startServerOrSkip(faults: faults)
        try launchApp()

        requireDeviceCard()
        let firstRow = Self.sampleIDs[0]  // 最新文件最大（660KB @4KB/s，下载可持续数十秒）
        XCTAssertTrue(
            waitForStatusPrefix(firstRow, "下载中"),
            "下载已开始（行进入下载中）"
        )
        XCTAssertTrue(waitForExistence("device.downloadProgress", timeout: 10), "进度条出现")

        element("device.cancelButton").click()
        XCTAssertTrue(
            waitForDisappearance("device.downloadProgress", timeout: 10),
            "进度条消失"
        )
        XCTAssertTrue(
            waitForStatus(firstRow, equal: "未下载", timeout: 10),
            "行状态回未下载"
        )
        XCTAssertTrue(
            waitForElement(
                "device.fileTable",
                predicate: NSPredicate(block: { _, _ in self.partialFileCount() == 0 }),
                timeout: 10
            ),
            ".partial 残留已清理"
        )
        XCTAssertEqual(libraryRecordingCount(), 0, "取消后无入库目录")
    }

    // MARK: - 6. 库流：同步入列 + 选择出现播放器（Phase C）

    /// 库流前置：供源 + 启动 + 切库页 + 首行转码完成（有界等待）
    /// 侧栏可见性保障（同骨架类）：小屏折叠时 ⌃⌘S 展开后再导航。
    private func ensureSidebarVisible() {
        let sidebar = element("sidebar.navItem.devices")
        if sidebar.waitForExistence(timeout: 3) { return }
        app.typeKey("s", modifierFlags: [.control, .command])
        _ = sidebar.waitForExistence(timeout: 5)
    }

    private func prepareLibraryWithReadyRows() throws {
        try startServerOrSkip()
        try launchApp()
        ensureSidebarVisible()
        element("sidebar.navItem.library").click()
        let first = Self.sampleIDs[0]
        XCTAssertTrue(
            waitForExistence("library.recordingTable.row.\(first)", timeout: 60),
            "同步后库列表行出现"
        )
        // 就绪以 test 侧 m4a 产物为准（自建 tmp 目录 runner 可读；避免徽标值轮询的 AX 时序脆性）
        let m4aURL = state.libraryRoot!.appendingPathComponent("Library/\(first)/audio.m4a")
        XCTAssertTrue(
            pollFileExists(m4aURL, timeout: 60),
            "首行转码完成（audio.m4a 产物出现）"
        )
    }

    func testSyncPopulatesLibraryAndSelects() throws {
        try prepareLibraryWithReadyRows()
        let first = Self.sampleIDs[0]
        element("library.recordingTable.row.\(first)").click()
        XCTAssertTrue(waitForExistence("library.player.playButton", timeout: 15), "详情播放器出现")
        XCTAssertTrue(waitForExistence("library.waveform", timeout: 15), "波形出现")
        XCTAssertTrue(waitForExistence("library.exportButton.m4a", timeout: 5))
        XCTAssertTrue(waitForExistence("library.exportButton.wav", timeout: 5))
        XCTAssertTrue(waitForExistence("library.exportButton.ogg", timeout: 5))
        XCTAssertTrue(waitForExistence("library.detailTabs", timeout: 5), "Tab 分段出现")
    }

    func testPlayPauseSeekAndSpeed() throws {
        try prepareLibraryWithReadyRows()
        let first = Self.sampleIDs[0]
        element("library.recordingTable.row.\(first)").click()
        XCTAssertTrue(waitForExistence("library.player.playButton", timeout: 15))

        element("library.player.playButton").click()
        XCTAssertTrue(
            waitForElement(
                "library.player.playButton",
                predicate: NSPredicate(format: "label == %@", "暂停"),
                timeout: 10
            ),
            "播放中按钮变「暂停」"
        )
        XCTAssertTrue(
            waitForElement(
                "library.player.progressSlider",
                predicate: NSPredicate(format: "value.doubleValue >= 0.02"),
                timeout: 30
            ),
            "进度增长"
        )
        element("library.player.fwd15Button").click()
        let seekOK = waitForElement(
            "library.player.timeLabel",
            predicate: NSPredicate(format: "value BEGINSWITH %@", "0:1"),
            timeout: 10
        )
        if !seekOK {
            let actual = element("library.player.timeLabel").exists
                ? (element("library.player.timeLabel").value ?? "nil")
                : "<timeLabel 不存在>"
            XCTFail("+15s 后时间进入 0:15~，实际 label=\(actual)")
        }
        element("library.player.speedButton").click()
        XCTAssertTrue(
            waitForElement(
                "library.player.speedButton",
                predicate: NSPredicate(format: "label == %@", "1.5×"),
                timeout: 5
            ),
            "倍速循环到 1.5×"
        )
    }

    // MARK: - 5. 空文件列表：fileTable 空态

    func testEmptyListShowsEmptyState() throws {
        var faults = SimFaults()
        faults.emptyFileList = true
        try startServerOrSkip(faults: faults)
        try launchApp()
        requireDeviceCard()
        XCTAssertTrue(
            waitForExistence("device.fileTable.emptyState", timeout: 10),
            "空文件列表显示 fileTable 空态"
        )
        XCTAssertTrue(waitForDisappearance("device.fileTable", timeout: 5), "无文件表")
    }
}

/// 进程内 Ogg/Opus 静音流生成（测试设备源；真实封装 + WebRTC 静音帧，
/// 转码链（demux→AVAudioConverter）全路径可消费）。
enum SyntheticOgg {
    /// 静音帧：TOC 0xF8 + 0xFF 0xFE（48kHz 单声道 20ms，Opus 解码器广泛接受）
    private static let silencePacket: [UInt8] = [0xF8, 0xFF, 0xFE]

    static func silence(durationSeconds: Int) -> Data {
        var id = Data([0x4F, 0x70, 0x75, 0x73, 0x48, 0x65, 0x61, 0x64]) // "OpusHead"
        id.append(1)                       // version
        id.append(1)                       // channels
        appendLE16(&id, 312)               // pre-skip
        appendLE32(&id, 48_000)            // input rate
        appendLE16(&id, 0)                 // output gain
        id.append(0)                       // mapping
        var tags = Data([0x4F, 0x70, 0x75, 0x73, 0x54, 0x61, 0x67, 0x73]) // "OpusTags"
        appendLE32(&tags, 0)               // vendor len 0
        appendLE32(&tags, 0)               // user comment list len 0

        let packetsPerSecond = 50
        let audioPackets = (0..<(durationSeconds * packetsPerSecond)).map { _ in Data(silencePacket) }

        var out = Data()
        out.append(page(serial: 1, sequence: 0, headerType: 0x02, segments: [id]))
        out.append(page(serial: 1, sequence: 1, headerType: 0, segments: [tags]))
        var sequence: UInt32 = 2
        var offset = 0
        while offset < audioPackets.count {
            let end = Swift.min(offset + 50, audioPackets.count)
            let isLast = end == audioPackets.count
            out.append(page(serial: 1, sequence: sequence, headerType: isLast ? 0x04 : 0, segments: Array(audioPackets[offset..<end])))
            sequence += 1
            offset = end
        }
        return out
    }

    private static func page(serial: UInt32, sequence: UInt32, headerType: UInt8, segments: [Data]) -> Data {
        var segTable: [UInt8] = []
        var payload = Data()
        for segment in segments {
            precondition(segment.count <= 255)
            segTable.append(UInt8(segment.count))
            payload.append(segment)
        }
        var page = Data()
        page.append(contentsOf: [0x4F, 0x67, 0x67, 0x53])
        page.append(0)
        page.append(headerType)
        appendLE64(&page, 0)
        appendLE32(&page, serial)
        appendLE32(&page, sequence)
        page.append(contentsOf: [0, 0, 0, 0])
        page.append(UInt8(segTable.count))
        page.append(contentsOf: segTable)
        page.append(payload)
        let crc = oggCRC(page)
        page[22] = UInt8(crc & 0xFF)
        page[23] = UInt8((crc >> 8) & 0xFF)
        page[24] = UInt8((crc >> 16) & 0xFF)
        page[25] = UInt8((crc >> 24) & 0xFF)
        return page
    }

    private static func appendLE16(_ data: inout Data, _ value: UInt16) {
        data.append(UInt8(value & 0xFF)); data.append(UInt8((value >> 8) & 0xFF))
    }

    private static func appendLE32(_ data: inout Data, _ value: UInt32) {
        for shift in [0, 8, 16, 24] { data.append(UInt8((value >> shift) & 0xFF)) }
    }

    private static func appendLE64(_ data: inout Data, _ value: UInt64) {
        for shift in [0, 8, 16, 24, 32, 40, 48, 56] { data.append(UInt8((value >> shift) & 0xFF)) }
    }

    private static func oggCRC(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0
        for (index, byte) in data.enumerated() {
            let value: UInt8 = (index >= 22 && index <= 25) ? 0 : byte
            crc = (crc << 8) ^ table[Int(UInt8((crc >> 24) ^ UInt32(value)))]
        }
        return crc
    }

    private static let table: [UInt32] = {
        (0..<256).map { index in
            var crc = UInt32(index) << 24
            for _ in 0..<8 {
                crc = (crc & 0x8000_0000) != 0 ? (crc << 1) ^ 0x04C1_1DB7 : crc << 1
            }
            return crc
        }
    }()
}

private extension Array {
    func chunked(into size: Int) -> [[Element]] {
        stride(from: 0, to: count, by: size).map { Array(self[$0..<Swift.min($0 + size, count)]) }
    }
}
