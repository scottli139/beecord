import AppKit
import BuzzLinkCore
import BuzzLinkSimCore
import XCTest

/// Accessibility Audit（T8 收口；studio-tech-design §6"Audit 无 error 级问题纳入 P0 验收"的
/// 机器化实现；审计规则对应 DESIGN_SYSTEM §4 R1-R10）。
///
/// **Phase 2（门禁，2026-10-18）**：Phase 1 侦察（handler 只记录不 fail）已回收 CI 全量
/// issue 清单（`/tmp/audit-round2.txt`，7 场景 129 行）并按清单逐项修复；修复后 handler
/// 变为**门禁**——命中静态豁免表的 issue 返回 true（抑制），其余一律返回 false（fail）。
/// 豁免表即"有意不修复"的唯一清单（DESIGN_SYSTEM §4 R10 豁免登记制度）：新增豁免必须
/// 在豁免表与 DESIGN_SYSTEM §4 R10 同步登记，并附中文理由。匹配规则：id 非空按
/// (type, id) 精确匹配；id 空按 (type, role, label) 签名匹配；role=0 表示无关联元素。
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

    // MARK: - 审计核心（Phase 2：豁免表门禁）

    /// 一条豁免规则。`identifier` 非空时按 (auditType, identifier) 精确匹配；`identifier`
    /// 为空时按 (auditType, role, label) 签名匹配；`role == 0` 仅用于无关联元素的 issue
    /// （issue.element == nil）。`reason` 为中文登记理由（DESIGN_SYSTEM §4 R10 豁免登记
    /// 制度——豁免表即"有意不修复"的唯一清单）。
    private struct AuditExemption {
        let auditType: UInt64
        let identifier: String
        let role: Int
        let label: String
        let reason: String

        init(type: UInt64, id: String = "", role: Int = 0, label: String = "", reason: String) {
            self.auditType = type
            self.identifier = id
            self.role = role
            self.label = label
            self.reason = reason
        }
    }

    /// 豁免表（Phase 2）。auditType 取值：1=对比度、8=描述类（无描述/不可读/Unknown role）、
    /// 4294967296=Action missing、8589934592=Parent/Child mismatch。
    private static let exemptions: [AuditExemption] = [
        // MARK: 对比度（type=1）——Mac 系统语义色 = 平台基准（R10）：浅色 secondary 实测
        // 3.44:1（WCAG AA 4.5 未达），与 Apple 自家 app 同水平；深色 secondary 实测
        // 5.3~6.0 达标。正文/状态信息的 tertiary 已按 R10 提级 text.secondary，残留项为
        // secondary 语义色本身的浅色差距。
        .init(type: 1, id: "sidebar.brand",
              reason: "品牌头文字 text.secondary：系统语义色=平台基准，深色实测 5.3~6.0 达标（R10）"),
        .init(type: 1, id: "sidebar.deviceStatus",
              reason: "脚标行文本为 labelColor 主色（达标）；报项为状态圆点（语义色指示：绿=已连接/"
              + "橙=蓝牙不可用/灰=断开，灰点已提级 secondary），图形指示符按语义色=平台基准豁免，深色达标（R10）"),
        .init(type: 1, id: "device.infoCard.storage",
              reason: "存储行文字 text.secondary：系统语义色=平台基准，深色实测 5.3~6.0 达标（R10）"),
        .init(type: 1, id: "device.fileTable.row.REC_20260103_074500.status",
              reason: "「未下载」状态徽标已提级 text.secondary（R10）：系统语义色=平台基准，深色实测达标"),
        .init(type: 1, id: "device.fileTable.row.REC_20260102_093000.status",
              reason: "「未下载」状态徽标已提级 text.secondary（R10）：系统语义色=平台基准，深色实测达标"),
        .init(type: 1, id: "device.fileTable.row.REC_20260101_120000.status",
              reason: "「未下载」状态徽标已提级 text.secondary（R10）：系统语义色=平台基准，深色实测达标"),
        .init(type: 1, id: "settings.downloadDir",
              reason: "「未选择」占位已提级 text.secondary（R10）：系统语义色=平台基准，深色实测达标"),
        .init(type: 1, id: "library.player.timeLabel",
              reason: "走带时间 text.secondary：系统语义色=平台基准，深色实测 5.3~6.0 达标（R10）"),
        .init(type: 1, id: "library.recordingCount",
              reason: "库列表头计数已提级 text.secondary（R10）：系统语义色=平台基准，深色实测达标"),
        .init(type: 1, id: "library.sortHint",
              reason: "库列表头排序提示已提级 text.secondary（R10）：系统语义色=平台基准，深色实测达标"),
        .init(type: 1, id: "library.recordingTable.row.REC_20260103_074500.meta",
              reason: "行 meta（日期·时长）已提级 text.secondary（R10）：系统语义色=平台基准，深色实测达标"),
        .init(type: 1, id: "library.recordingTable.row.REC_20260102_093000.meta",
              reason: "行 meta（日期·时长）已提级 text.secondary（R10）：系统语义色=平台基准，深色实测达标"),
        .init(type: 1, id: "library.recordingTable.row.REC_20260101_120000.meta",
              reason: "行 meta（日期·时长）已提级 text.secondary（R10）：系统语义色=平台基准，深色实测达标"),
        .init(type: 1, role: 48, label: "",
              reason: "匿名正文/状态文本 text.secondary（含设置页转写组 P1 占位 disabled 灰化文本）："
              + "系统语义色/系统禁用态=平台基准，深色实测达标（R10）"),

        // MARK: 描述类（type=8）——框架生成容器/系统私有控件，无业务语义。
        .init(type: 8, role: 3, label: "",
              reason: "匿名 group 无描述：SwiftUI 容器（VStack/HStack/ScrollView 等）框架生成，无业务语义"),
        .init(type: 8, role: 1, label: "",
              reason: "匿名 window/other 无描述：窗口与场景层级框架生成，无业务语义"),
        .init(type: 8, role: 81, label: "",
              reason: "装饰性 SF Symbol 图标（空态/信息卡/电池等）无描述：信息由伴随文本承担，图形无业务语义"),
        .init(type: 8, role: 14, label: "emoji & symbols",
              reason: "AppKit 文本字段私有字符选择器按钮（系统生成，公开 API 无法定制/隐藏）：非业务元素"),
        .init(type: 8, id: "library.waveform",
              reason: "自绘波形（R5 accessibilityElement）：已补 isImage trait；若仍报 Unknown role，系 "
              + "XCTAudit 对自定义图形元素的已知限制"),

        // MARK: Action missing（type=4294967296）——系统控件 AX action 由 AppKit 管理，
        // XCTAudit 对系统弹出按钮/菜单触发钮误报；可交互性由既有功能用例覆盖。
        .init(type: 4294967296, role: 14, label: "emoji & symbols",
              reason: "AppKit 文本字段私有字符选择器按钮（系统生成）：同 type=8 条目"),
        .init(type: 4294967296, id: "settings.transportPicker",
              reason: "系统 Form Picker（框架控件）：弹出按钮 AX action 由 AppKit 管理，XCTAudit 对系统控件误报；"
              + "可交互性由既有功能用例覆盖"),
        .init(type: 4294967296, id: "settings.aiProviderPicker",
              reason: "系统 Form Picker（框架控件）：弹出按钮 AX action 由 AppKit 管理，XCTAudit 对系统控件误报；"
              + "可交互性由既有功能用例覆盖"),
        .init(type: 4294967296, id: "library.exportButton",
              reason: "系统 Menu 触发钮（框架控件）：同上报；导出交互由 DeviceSyncUITests 覆盖"),

        // MARK: Parent/Child mismatch（type=8589934592）——框架容器，无业务语义。
        .init(type: 8589934592, role: 3, label: "",
              reason: "Parent/Child mismatch：SwiftUI 容器分组框架生成，无业务语义"),
        .init(type: 8589934592, role: 0, label: "",
              reason: "Parent/Child mismatch（无关联元素）：窗口/场景层级框架生成，无业务语义"),
    ]

    private static func matchExemption(_ issue: XCUIAccessibilityAuditIssue) -> AuditExemption? {
        for exemption in exemptions where exemption.auditType == issue.auditType.rawValue {
            if let element = issue.element {
                if !exemption.identifier.isEmpty {
                    if element.identifier == exemption.identifier { return exemption }
                } else if exemption.role == element.elementType.rawValue
                            && exemption.label == element.label {
                    return exemption
                }
            } else if exemption.identifier.isEmpty && exemption.role == 0 && exemption.label.isEmpty {
                return exemption
            }
        }
        return nil
    }

    /// 对当前可见 UI 执行全量审计（`performAccessibilityAudit` 是 XCUIApplication 级 API，
    /// 覆盖应用全部可见元素；场景由前置等待的 AX id 保证在场）。
    /// Phase 2（门禁）：handler 逐条 print 后按豁免表判定——命中豁免返回 true（抑制），
    /// 未命中返回 false（该 issue 使用例失败）。
    private func audit(_ sceneTag: String) throws {
        print("AUDIT[\(sceneTag)] begin")
        try app.performAccessibilityAudit(for: .all) { issue in
            let elDesc: String
            if let el = issue.element {
                elDesc = "[id=\(el.identifier) label=\(el.label) role=\(el.elementType.rawValue)]"
            } else {
                elDesc = "[no-element]"
            }
            print("AUDIT[\(sceneTag)] type=\(issue.auditType.rawValue) "
                  + "desc=\(issue.compactDescription) el=\(elDesc)")
            guard let exemption = Self.matchExemption(issue) else {
                print("AUDIT[\(sceneTag)] MISS ↑ 未豁免，按失败处理")
                return false
            }
            print("AUDIT[\(sceneTag)] HIT ↑ 豁免：\(exemption.reason)")
            return true
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
