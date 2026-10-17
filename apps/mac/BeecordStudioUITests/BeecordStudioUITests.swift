import XCTest

/// App 骨架 UI 测试（TESTING.md §4：AX 树驱动，禁坐标/截图；R1 id 命名）。
@MainActor
final class BeecordStudioUITests: XCTestCase {

    let app = XCUIApplication()

    override func setUp() async throws {
        continueAfterFailure = false
        // 确定性空库：隔离默认容器残留数据（本机曾手动同步过录音，空态断言会受污染）
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("studio-shell-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        app.launchArguments = ["-resetSettings", "-libraryRoot", root.path]
    }

    /// 启动并保障侧栏可见（小屏折叠时 ⌃⌘S 展开）。
    private func launchWithSidebar() {
        app.launch()
        ensureSidebarVisible()
    }

    /// 侧栏可见性保障：CI runner 早期小屏可能折叠 NavigationSplitView 侧栏（
    /// 窗口被 AppKit 收到屏宽以下）——缺席时按标准 ⌃⌘S 展开（SidebarCommands）。
    func ensureSidebarVisible() {
        if element("sidebar.navItem.devices").waitForExistence(timeout: 3) { return }
        app.typeKey("s", modifierFlags: [.control, .command])
        _ = element("sidebar.navItem.devices").waitForExistence(timeout: 5)
    }

    /// 类型无关的 AX id 查询（兼容 checkBox/switch/staticText 等角色差异）。
    private func element(_ identifier: String) -> XCUIElement {
        app.descendants(matching: .any).matching(identifier: identifier).firstMatch
    }

    private func openSettings() {
        app.typeKey(",", modifierFlags: .command)
    }

    /// 开关值断言：兼容 macOS AXValue 的 String/NSNumber 表示。
    private func assertToggleValue(_ element: XCUIElement, expected: Bool, _ message: String) {
        let actual: Bool? = switch element.value {
        case let text as String: text == "1" || text == "true"
        case let number as NSNumber: number.boolValue
        default: nil
        }
        XCTAssertEqual(actual, expected, message)
    }

    // MARK: - 1. 启动：侧栏 + 设备页空态，控制台默认隐藏

    func testLaunchShowsSidebarAndDevicePage() throws {
        launchWithSidebar()
        XCTAssertTrue(element("sidebar.navItem.devices").waitForExistence(timeout: 10))
        XCTAssertTrue(element("sidebar.navItem.library").exists)
        XCTAssertFalse(element("sidebar.navItem.console").exists)
        XCTAssertTrue(element("device.emptyState").exists)
    }

    // MARK: - 2. 侧栏导航切页 + ⌘1/⌘2 快捷键

    func testSidebarNavigationSwitchesPages() throws {
        launchWithSidebar()
        element("sidebar.navItem.library").click()
        XCTAssertTrue(element("library.emptyState").waitForExistence(timeout: 5))
        app.typeKey("1", modifierFlags: .command)
        XCTAssertTrue(element("device.emptyState").waitForExistence(timeout: 5))
        app.typeKey("2", modifierFlags: .command)
        XCTAssertTrue(element("library.emptyState").waitForExistence(timeout: 5))
    }

    // MARK: - 3. 设置窗口（⌘,）：分组与默认值

    func testSettingsWindowOpensAndGroupsExist() throws {
        launchWithSidebar()
        openSettings()
        let autoSync = element("settings.autoSyncToggle")
        XCTAssertTrue(autoSync.waitForExistence(timeout: 10))
        assertToggleValue(autoSync, expected: true, "自动同步默认开")
        let developerMode = element("settings.developerModeToggle")
        XCTAssertTrue(developerMode.exists)
        assertToggleValue(developerMode, expected: false, "开发者模式默认关")
        XCTAssertTrue(element("settings.downloadDir").exists)
        XCTAssertTrue(element("settings.downloadDir.chooseButton").exists)
        XCTAssertTrue(element("settings.aiProviderPicker").exists)
        XCTAssertTrue(element("settings.aiBaseURL").exists)
        XCTAssertTrue(element("settings.aiAPIKey").exists)
        XCTAssertTrue(element("settings.aiModel").exists)
        XCTAssertTrue(element("settings.aiFeature.summary").exists)
        XCTAssertTrue(element("settings.aiFeature.meeting").exists)
        XCTAssertTrue(element("settings.aiFeature.todo").exists)
        XCTAssertTrue(element("settings.aiFeature.translate").exists)
        XCTAssertTrue(element("settings.aiTestButton").exists)
        XCTAssertTrue(element("settings.whisper.model").exists)
        XCTAssertTrue(element("settings.whisper.manageButton").exists)
        XCTAssertTrue(element("settings.whisper.language").exists)
        XCTAssertTrue(element("settings.about.version").exists)
        XCTAssertTrue(element("settings.about.protocolVersion").exists)
        XCTAssertTrue(element("settings.about.license").exists)
        XCTAssertTrue(element("settings.about.tagline").exists)
    }

    // MARK: - 4. 开发者模式门控控制台

    func testDeveloperModeRevealsConsole() throws {
        launchWithSidebar()
        XCTAssertFalse(element("sidebar.navItem.console").exists)
        openSettings()
        let toggle = element("settings.developerModeToggle")
        XCTAssertTrue(toggle.waitForExistence(timeout: 10))
        toggle.click()
        app.typeKey("w", modifierFlags: .command)
        let consoleItem = element("sidebar.navItem.console")
        XCTAssertTrue(consoleItem.waitForExistence(timeout: 5))
        consoleItem.click()
        XCTAssertTrue(element("console.emptyState").waitForExistence(timeout: 5))
        // 复位：关开发者模式，侧栏入口消失
        openSettings()
        element("settings.developerModeToggle").click()
        app.typeKey("w", modifierFlags: .command)
        XCTAssertFalse(element("sidebar.navItem.console").waitForExistence(timeout: 5))
    }
}
