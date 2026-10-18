import AppKit
import BuzzLinkCore
import BuzzLinkSimCore
import XCTest

/// 协议控制台 UI 测试（T7 Phase C；TESTING.md §4：AX 树驱动）。
/// 设备源 = 进程内 UITestSimServer（loopback 40404，同 DeviceSyncUITests）；
/// 每用例独立库根 + `-consoleExportPath` 导出断言路径；控制台入口受开发者模式门控，
/// 用例先经设置开关开启（同 testDeveloperModeRevealsConsole 路径）。
/// 慢 runner 纪律：短驻留断言紧跟交互；全部判定有界轮询，禁固定 sleep。
@MainActor
final class ConsoleUITests: XCTestCase {

    private var app: XCUIApplication!
    private var server: UITestSimServer?
    private var libraryRoot: URL?
    private var exportPath: URL?

    override func tearDownWithError() throws {
        server?.stop()
        server = nil
        if let libraryRoot {
            try? FileManager.default.removeItem(at: libraryRoot)
        }
    }

    // MARK: - 基础设施

    private func element(_ identifier: String) -> XCUIElement {
        app.descendants(matching: .any).matching(identifier: identifier).firstMatch
    }

    @discardableResult
    private func waitForExistence(_ identifier: String, timeout: TimeInterval = 15) -> Bool {
        element(identifier).waitForExistence(timeout: timeout)
    }

    @discardableResult
    private func waitForElement(
        _ identifier: String,
        predicate: NSPredicate,
        timeout: TimeInterval = 15
    ) -> Bool {
        let expectation = XCTNSPredicateExpectation(predicate: predicate, object: element(identifier))
        return XCTWaiter().wait(for: [expectation], timeout: timeout) == .completed
    }

    /// 日志可见条目数 = 个体行 + 聚合行（console.logTable.row.* / .agg.*）。
    /// 聚合行不算 .row. 前缀——勾选 DL_CHUNK 等过滤后个体行被折叠成 agg 行，
    /// 只统计 .row. 会得到 0（CI run 39100523966 :199 实录）。
    /// 注：.row.N.hex 展开区仅展开态存在，本用例不展开行。
    private func visibleLogCount() -> Int {
        let rows = app.descendants(matching: .any)
            .matching(NSPredicate(format: "identifier BEGINSWITH %@", "console.logTable.row."))
        let aggs = app.descendants(matching: .any)
            .matching(NSPredicate(format: "identifier BEGINSWITH %@", "console.logTable.agg."))
        return rows.count + aggs.count
    }

    /// 有界等待任一**个体行或聚合行**的 AX label 含给定文本（R4 可读文本；
    /// 聚合行 label 形如 "DL_CHUNK ×2,351（已折叠，点击看明细）"，同样含 type 名）
    private func waitForRowContaining(_ text: String, timeout: TimeInterval = 15) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            let rows = app.descendants(matching: .any)
                .matching(NSPredicate(format: "identifier BEGINSWITH %@", "console.logTable.row."))
            let aggs = app.descendants(matching: .any)
                .matching(NSPredicate(format: "identifier BEGINSWITH %@", "console.logTable.agg."))
            for index in 0..<min(rows.count, 60) {
                if rows.element(boundBy: index).label.contains(text) { return true }
            }
            for index in 0..<min(aggs.count, 20) {
                if aggs.element(boundBy: index).label.contains(text) { return true }
            }
            Thread.sleep(forTimeInterval: 0.2)
        }
        return false
    }

    private func pollFileExists(_ url: URL, timeout: TimeInterval = 10) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if FileManager.default.fileExists(atPath: url.path) { return true }
            Thread.sleep(forTimeInterval: 0.2)
        }
        return FileManager.default.fileExists(atPath: url.path)
    }

    /// 启动进程内 Sim server（端口占用 → XCTSkip；同 DeviceSyncUITests）
    private func startServerOrSkip() throws {
        let files = [
            SimFile(id: "REC_20260103_074500", durationMs: 5_000, data: SyntheticOgg.silence(durationSeconds: 5)),
            SimFile(id: "REC_20260102_093000", durationMs: 30_000, data: SyntheticOgg.silence(durationSeconds: 30)),
            SimFile(id: "REC_20260101_120000", durationMs: 120_000, data: SyntheticOgg.silence(durationSeconds: 120))
        ]
        let server = UITestSimServer(engine: SimEngine(files: files))
        for attempt in 1...3 {
            do {
                try server.start()
                self.server = server
                return
            } catch {
                if attempt == 3 {
                    throw XCTSkip("UITestSimServer 无法启动：\(error.localizedDescription)")
                }
                RunLoop.current.run(until: Date().addingTimeInterval(0.5))
            }
        }
    }

    /// 启动并保障主窗口 + 侧栏；注入独立库根与导出路径
    private func launchWithSidebar() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("console-uitest-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        libraryRoot = root
        exportPath = root.appendingPathComponent("console-export.json")
        app = XCUIApplication()
        app.launchArguments = [
            "-resetSettings", "-libraryRoot", root.path,
            "-consoleExportPath", exportPath!.path,
            "-uiTestWindow"
        ]
        app.launch()
        app.activate()
        // 兜底：主窗口未随启动出现时 activate + ⌘N 强制新建（CI 认知第 2 条）
        if !element("sidebar.navItem.devices").waitForExistence(timeout: 10) {
            app.activate()
            app.typeKey("n", modifierFlags: .command)
            _ = element("sidebar.navItem.devices").waitForExistence(timeout: 5)
        }
        ensureSidebarVisible()
    }

    private func ensureSidebarVisible() {
        let sidebar = element("sidebar.navItem.devices")
        if sidebar.waitForExistence(timeout: 3) { return }
        app.typeKey("s", modifierFlags: [.control, .command])
        _ = sidebar.waitForExistence(timeout: 5)
    }

    /// 开启开发者模式（门控）并切到控制台页；等设备连接（发送门控解除）
    private func openConsoleConnected() throws {
        try startServerOrSkip()
        try launchWithSidebar()
        // 设置 → 开发者模式 → 关窗（同 testDeveloperModeRevealsConsole）
        app.typeKey(",", modifierFlags: .command)
        XCTAssertTrue(waitForExistence("settings.developerModeToggle", timeout: 10), "设置窗口打开")
        element("settings.developerModeToggle").click()
        app.typeKey("w", modifierFlags: .command)
        XCTAssertTrue(waitForExistence("sidebar.navItem.console", timeout: 10), "开发者模式后门控开启")
        app.typeKey("3", modifierFlags: .command)
        XCTAssertTrue(waitForExistence("console.logTable", timeout: 10), "控制台页出现")
        // 等连接：发送门控提示消失（自动连接 loopback Sim 后 state ≠ idle）
        let connected = waitForElement(
            "console.sendButton",
            predicate: NSPredicate(block: { [weak self] _, _ in
                guard let self else { return false }
                return !self.element("console.sendGateTip").exists
            }),
            timeout: 60
        )
        XCTAssertTrue(connected, "设备连接且同步完成（发送门控解除）")
    }

    // MARK: - 1. 帧日志与三过滤

    func testConsoleLogsFramesAndFilters() throws {
        try openConsoleConnected()
        // 连接即自动同步——帧流自然产生；等有界行数 ≥4
        let enough = waitForElement(
            "console.logTable",
            predicate: NSPredicate(block: { [weak self] _, _ in (self?.visibleLogCount() ?? 0) >= 4 }),
            timeout: 30
        )
        XCTAssertTrue(enough, "自动同步应产生 ≥4 行帧日志")
        let baseline = visibleLogCount()

        // 方向过滤 → 仅上行（segmented 项有 AX id，本机验证可达；行数单调收敛）
        element("console.directionFilter.tx").click()
        _ = waitForElement(
            "console.logTable",
            predicate: NSPredicate(block: { [weak self] _, _ in
                guard let self else { return false }
                let count = self.visibleLogCount()
                return count > 0 && count <= baseline
            }),
            timeout: 10
        )
        let txOnly = visibleLogCount()
        XCTAssertGreaterThan(txOnly, 0, "上行过滤后仍有帧")
        XCTAssertLessThanOrEqual(txOnly, baseline, "方向过滤后行数不增")

        // 恢复全部方向；type 过滤勾选 DL_CHUNK（内联面板项按 AX id 查询——菜单/popover
        // 弹层的 AX 查询在 CI/本机均不稳，run 39098188609 实录）
        element("console.directionFilter.all").click()
        element("console.typeFilter").click()
        XCTAssertTrue(waitForExistence("console.typeFilter.item.DL_CHUNK", timeout: 5),
                      "type 面板展开含 DL_CHUNK 项")
        element("console.typeFilter.item.DL_CHUNK").click()
        let chunkFiltered = waitForElement(
            "console.logTable",
            predicate: NSPredicate(block: { [weak self] _, _ in
                guard let self else { return false }
                let count = self.visibleLogCount()
                return count > 0 && count <= baseline
            }),
            timeout: 10
        )
        XCTAssertTrue(chunkFiltered, "type 过滤后行数有界收敛")
        XCTAssertTrue(waitForRowContaining("DL_CHUNK", timeout: 5), "过滤后日志含 DL_CHUNK")

        // 文本过滤叠加 "dl"（type 名/注释/payload 子串）：行数进一步收敛或持平
        element("console.filterField").click()
        element("console.filterField").typeText("dl")
        _ = waitForElement(
            "console.logTable",
            predicate: NSPredicate(block: { [weak self] _, _ in
                guard let self else { return false }
                let count = self.visibleLogCount()
                return count > 0 && count <= baseline
            }),
            timeout: 10
        )
        // 恢复：清文本过滤 + 取消 type 勾选（面板仍展开——项点击即切换，无需先展开）
        element("console.filterField").click()
        if let stringValue = element("console.filterField").value as? String, !stringValue.isEmpty {
            element("console.filterField").typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: stringValue.count))
        }
        element("console.typeFilter.item.DL_CHUNK").click()
        // done 有界等待再点（面板 LazyVGrid 重建后 done 可能短暂缺席，CI :220 实录）
        XCTAssertTrue(waitForExistence("console.typeFilter.done", timeout: 5), "type 面板完成钮在场")
        element("console.typeFilter.done").click()
    }

    // MARK: - 2. 预置命令：列文件 → LIST_REQ(TX) + LIST_RESP(RX)

    func testConsolePresetCommand() throws {
        try openConsoleConnected()
        element("console.clearButton").click()
        element("console.presetButton.list").click()
        XCTAssertTrue(
            waitForRowContaining("LIST_REQ", timeout: 15),
            "预置「列文件」应发出 LIST_REQ（TX 行）"
        )
        XCTAssertTrue(
            waitForRowContaining("LIST_RESP", timeout: 15),
            "设备应回 LIST_RESP（RX 行）"
        )
    }

    // MARK: - 3. 自定义 hex：PING → TX 行 + RX PONG（响应经旁听流）

    func testConsoleCustomHex() throws {
        try openConsoleConnected()
        element("console.clearButton").click()
        // PING 整帧：beec | ver=01 | type=01 | session=2A | flags=00 | chan=0000 | len=0000 | crc=0000
        //（CRC 占位即可——控制台重建帧时重算；校验只看魔数/长度/hex 合法性）
        element("console.hexField").click()
        element("console.hexField").typeText("beec01012a00000000000000")
        element("console.sendButton").click()
        XCTAssertTrue(
            waitForRowContaining("PING", timeout: 15),
            "自定义 hex 应产生 PING（TX 行）"
        )
        XCTAssertTrue(
            waitForRowContaining("PONG", timeout: 15),
            "设备应回 PONG（RX 行；响应高亮无 AX 通道，行存在即反馈）"
        )
    }

    // MARK: - 4. 导出：注入路径直写 + 测试侧解码 buzzlink-vector/1

    func testConsoleExport() throws {
        try openConsoleConnected()
        element("console.exportButton").click()
        XCTAssertTrue(pollFileExists(exportPath!), "导出文件应写入注入路径")
        let data = try Data(contentsOf: exportPath!)
        let vector = try JSONDecoder().decode(BuzzLinkTestVector.self, from: data)
        XCTAssertEqual(vector.schema, "buzzlink-vector/1")
        XCTAssertGreaterThanOrEqual(vector.entries.count, 2, "导出至少含一对请求/响应")
        XCTAssertTrue(vector.entries.contains { $0.direction == .tx }, "导出含 TX 帧")
        XCTAssertTrue(vector.entries.contains { $0.direction == .rx }, "导出含 RX 帧")
    }
}
