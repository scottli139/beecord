import AppKit
import BuzzLinkCore
import BuzzLinkSimCore
import XCTest

/// Accessibility Audit（T8 收口；studio-tech-design §6"Audit 无 error 级问题纳入 P0 验收"的
/// 机器化实现；审计规则对应 DESIGN_SYSTEM §4 R1-R9）。
///
/// **Phase 1（侦察）**：completion handler 对所有 issue 只记录不 fail（恒 return true），
/// issue 的 type/描述 print 到测试日志（CI 日志回收清单）；Phase 2 按清单修复后，
/// handler 仅对已豁免项（有意设计，中文注释说明）返回 true，其余 fail。
///
/// 场景：设备页空态/连接态、库页空态/有文件/播放器展开、协议控制台（开发者模式门控）、
/// 设置窗口。设备源 = 进程内 UITestSimServer（同 DeviceSyncUITests/ConsoleUITests 惯例）。
@MainActor
final class AccessibilityAuditUITests: XCTestCase {

    private var app: XCUIApplication!
    private var server: UITestSimServer?
    private var libraryRoot: URL?

    override func tearDownWithError() throws {
        server?.stop()
        server = nil
        if let libraryRoot {
            try? FileManager.default.removeItem(at: libraryRoot)
        }
    }

    // MARK: - 基建（与 DeviceSyncUITests/ConsoleUITests 同款模式）

    private func element(_ identifier: String) -> XCUIElement {
        app.descendants(matching: .any).matching(identifier: identifier).firstMatch
    }

    @discardableResult
    private func waitForExistence(_ identifier: String, timeout: TimeInterval = 15) -> Bool {
        element(identifier).waitForExistence(timeout: timeout)
    }

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

    /// 启动并保障主窗口 + 侧栏；每用例独立库根
    private func launchWithSidebar() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("audit-uitest-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        libraryRoot = root
        app = XCUIApplication()
        app.launchArguments = ["-resetSettings", "-libraryRoot", root.path, "-uiTestWindow"]
        app.launch()
        app.activate()
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

    /// 开发者模式门控（控制台入口前置；同 ConsoleUITests 现成路径）
    private func enableDeveloperMode() {
        app.typeKey(",", modifierFlags: .command)
        XCTAssertTrue(waitForExistence("settings.developerModeToggle", timeout: 10), "设置窗口打开")
        element("settings.developerModeToggle").click()
        app.typeKey("w", modifierFlags: .command)
        XCTAssertTrue(waitForExistence("sidebar.navItem.console", timeout: 10), "开发者模式门控开启")
    }

    // MARK: - 审计核心（Phase 1：只记录不 fail）

    /// 对当前可见 UI 执行全量审计（`performAccessibilityAudit` 是 XCUIApplication 级 API，
    /// 覆盖应用全部可见元素；场景由前置等待的 AX id 保证在场）。
    /// Phase 1（侦察）：handler 逐条 print 后恒抑制；Phase 2 收口后仅豁免项返回 true，其余 fail。
    private func audit(_ sceneTag: String) throws {
        print("AUDIT[\(sceneTag)] begin")
        try app.performAccessibilityAudit(for: .all) { issue in
            // Phase 1（侦察）：全量记录。element 的 identifier/label/elementType 用于定位修复点。
            // element 为可选（部分 issue 不关联具体元素），缺省时打占位。
            let elDesc: String
            if let el = issue.element {
                elDesc = "[id=\(el.identifier) label=\(el.label) role=\(el.elementType.rawValue)]"
            } else {
                elDesc = "[no-element]"
            }
            print("AUDIT[\(sceneTag)] type=\(issue.auditType.rawValue) "
                  + "desc=\(issue.compactDescription) el=\(elDesc)")
            return true   // TODO(Phase 2)：已修复项改 return false；有意设计项在此精确豁免并注释理由
        }
        print("AUDIT[\(sceneTag)] end")
    }

    // MARK: - 场景 1：设备页空态

    func testAuditDevicePageEmpty() throws {
        try launchWithSidebar()
        XCTAssertTrue(waitForExistence("device.emptyState", timeout: 10), "设备页空态在场")
        try audit("device-empty")
    }

    // MARK: - 场景 2：设备页连接态（文件列表）

    func testAuditDevicePageConnected() throws {
        try startServerOrSkip()
        try launchWithSidebar()
        XCTAssertTrue(
            waitForExistence("device.fileTable.row.REC_20260103_074500", timeout: 30),
            "连接后文件行出现"
        )
        try audit("device-connected")
    }

    // MARK: - 场景 3：库页空态

    func testAuditLibraryEmpty() throws {
        try launchWithSidebar()
        app.typeKey("2", modifierFlags: .command)
        XCTAssertTrue(waitForExistence("library.emptyState", timeout: 10), "库页空态在场")
        try audit("library-empty")
    }

    // MARK: - 场景 4：库页有文件态

    func testAuditLibraryWithItems() throws {
        try startServerOrSkip()
        try launchWithSidebar()
        app.typeKey("2", modifierFlags: .command)
        XCTAssertTrue(
            waitForExistence("library.recordingTable.row.REC_20260103_074500", timeout: 30),
            "同步后库列表行出现"
        )
        try audit("library-items")
    }

    // MARK: - 场景 5：库页播放器展开（选中已就绪行）

    func testAuditLibraryPlayer() throws {
        try startServerOrSkip()
        try launchWithSidebar()
        app.typeKey("2", modifierFlags: .command)
        let first = "REC_20260103_074500"
        XCTAssertTrue(
            waitForExistence("library.recordingTable.row.\(first)", timeout: 30),
            "库列表行出现"
        )
        // 就绪以 m4a 产物为准（自建 tmp 目录 runner 可读；同 DeviceSyncUITests 惯例）
        let m4aURL = libraryRoot!.appendingPathComponent("Library/\(first)/audio.m4a")
        let m4aReady = waitForElement(
            "library.recordingTable.row.\(first)",
            predicate: NSPredicate(block: { _, _ in
                FileManager.default.fileExists(atPath: m4aURL.path)
            }),
            timeout: 60
        )
        XCTAssertTrue(m4aReady, "首行转码就绪")
        element("library.recordingTable.row.\(first)").click()
        XCTAssertTrue(waitForExistence("library.player.playButton", timeout: 15), "详情播放器出现")
        try audit("library-player")
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

    // MARK: - 场景 6：协议控制台（开发者模式门控）

    func testAuditConsole() throws {
        try startServerOrSkip()
        try launchWithSidebar()
        enableDeveloperMode()
        app.typeKey("3", modifierFlags: .command)
        XCTAssertTrue(waitForExistence("console.logTable", timeout: 10), "控制台页出现")
        try audit("console")
    }

    // MARK: - 场景 7：设置窗口

    func testAuditSettingsWindow() throws {
        try launchWithSidebar()
        app.typeKey(",", modifierFlags: .command)
        XCTAssertTrue(waitForExistence("settings.developerModeToggle", timeout: 10), "设置窗口打开")
        try audit("settings")
        app.typeKey("w", modifierFlags: .command)
    }
}
