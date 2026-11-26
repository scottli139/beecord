import AppKit
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
        app.launchArguments = ["-resetSettings", "-libraryRoot", root.path, "-uiTestWindow"]
    }

    /// 启动并保障主窗口存在 + 侧栏可见。
    private func launchWithSidebar() {
        dismissSystemBluetoothAlert()
        app.launch()
        app.activate()
        diag("after-launch")
        // 兜底：主窗口未随启动出现（无恢复态冷启动等）时，activate + ⌘N 强制新建主窗口。
        // 2026-10-18 Window 单例场景改造后：主窗由 AppDelegate 自动补建，本兜底实际不再
        // 触发——保留作防御（Window 场景下 ⌘N 为无害 no-op；回退 WindowGroup 仍有效）。
        // （原注：CI 上 WindowGroup 偶发不自动建主窗）
        if !element("sidebar.navItem.devices").waitForExistence(timeout: 10) {
            app.activate()
            app.typeKey("n", modifierFlags: .command)
            _ = element("sidebar.navItem.devices").waitForExistence(timeout: 5)
        }
        diag("after-window-recovery")
        ensureSidebarVisible()
    }

    /// CI 取证诊断：打印 runner 侧屏幕与 app 窗口实况到测试日志（print 直达 CI log）。
    /// 定位"CI 早期侧栏缺席"用（本地不触发也无害），收敛后可移除。
    private func diag(_ tag: String) {
        let full = NSScreen.main?.frame ?? .zero
        let vis = NSScreen.main?.visibleFrame ?? .zero
        let winCount = app.windows.count
        var winFrame = CGRect.zero
        if winCount > 0 {
            winFrame = app.windows.firstMatch.frame
        }
        let sidebar = element("sidebar.navItem.devices").exists
        print("UITEST-DIAG[\(tag)] screen=\(full) visible=\(vis) state=\(app.state.rawValue) windows=\(winCount) winFrame=\(winFrame) sidebarDevices=\(sidebar)")
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

    /// 打开设置窗。原实现 typeKey(⌘,) 在本机失效（canary 对照实验：注入的
    /// 键事件全程未到达应用事件循环，macOS 26.5 + XCTest 环境问题，应用侧
    /// 无法接不存在的事件）；改用应用菜单"设置…"AX 点击——与真实用户
    /// 菜单路径一致，且不受键盘注入环境影响。⌘, 快捷键对真实键盘保留
    /// （AppDelegate 事件监视器 + 菜单等价，见 App 文件头注记）。
    private func openSettings() {
        let appMenu = app.menuBars.menuBarItems.element(boundBy: 1) // 应用菜单（0 是 Apple）
        appMenu.click()
        let item = appMenu.menuItems.matching(
            NSPredicate(format: "title CONTAINS '设置' OR title CONTAINS 'Settings'")
        ).firstMatch
        XCTAssertTrue(item.waitForExistence(timeout: 5), "应用菜单应含设置项")
        item.click()
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

    // MARK: - 4. 侧栏设置入口

    func testSidebarSettingsButtonOpensSettingsWindow() throws {
        launchWithSidebar()
        let settingsButton = element("sidebar.settingsButton")
        XCTAssertTrue(settingsButton.waitForExistence(timeout: 5), "侧栏应显示设置按钮")
        settingsButton.click()
        XCTAssertTrue(
            element("settings.autoSyncToggle").waitForExistence(timeout: 5),
            "点击侧栏设置按钮应打开设置窗口"
        )
    }

    // MARK: - 5. 切换连接方式后重建传输层

    func testTransportModeChangeRebuildsDeviceStack() throws {
        launchWithSidebar()
        openSettings()
        let picker = element("settings.transportPicker")
        XCTAssertTrue(picker.waitForExistence(timeout: 5), "设置窗口应显示连接方式")
        picker.click()
        let bleItem = app.menuItems["蓝牙（BLE）"]
        XCTAssertTrue(bleItem.waitForExistence(timeout: 3), "连接方式菜单应包含蓝牙")
        bleItem.click()

        XCTAssertTrue(
            element("device.transportMode.ble").waitForExistence(timeout: 10),
            "切换蓝牙后应重建并显示实际生效的 BLE 传输层"
        )
        // 首次实例化 CBCentralManager 触发系统授权弹窗（异步出现），有界等待并消除，
        // 避免滞留盖住后续用例控件
        dismissSystemBluetoothAlert(timeout: 15)
    }

    // MARK: - 6. 录音详情展开不改变窗口与侧栏几何

    func testLibrarySelectionKeepsWindowAndSidebarAnchored() throws {
        guard let rootIndex = app.launchArguments.firstIndex(of: "-libraryRoot"),
              app.launchArguments.indices.contains(rootIndex + 1) else {
            XCTFail("缺少测试库根参数")
            return
        }
        let root = URL(fileURLWithPath: app.launchArguments[rootIndex + 1], isDirectory: true)
        for index in 0..<1 {
            let id = String(format: "REC_LAYOUT_%02d", index)
            let recordingURL = root.appendingPathComponent("Library/\(id)", isDirectory: true)
            try FileManager.default.createDirectory(at: recordingURL, withIntermediateDirectories: true)
            let meta: [String: Any] = [
                "schemaVersion": 2,
                "id": id,
                "title": "布局回归录音 \(index + 1)",
                "source": [
                    "fileID": id, "deviceName": "布局设备",
                    "deviceModel": "BCD", "fwVersion": "1.0"
                ],
                "durationMs": 120_000,
                "sizeBytes": 0,
                "crc16": 0,
                "format": "ogg-opus",
                "sampleRate": 16_000,
                "channels": 1,
                "importedAt": String(format: "2026-11-25T09:%02d:00Z", index),
                "audio": ["ogg": "audio.ogg", "m4a": "audio.m4a"],
                "transcribe": ["status": "none"],
                "summary": ["status": "none"]
            ]
            try JSONSerialization.data(withJSONObject: meta, options: [.sortedKeys])
                .write(to: recordingURL.appendingPathComponent("meta.json"))
            XCTAssertTrue(FileManager.default.createFile(
                atPath: recordingURL.appendingPathComponent("audio.ogg").path, contents: Data()
            ))
            XCTAssertTrue(FileManager.default.createFile(
                atPath: recordingURL.appendingPathComponent("audio.m4a").path, contents: Data()
            ))
        }

        launchWithSidebar()
        element("sidebar.navItem.library").click()
        let row = element("library.recordingTable.row.REC_LAYOUT_00")
        XCTAssertTrue(row.waitForExistence(timeout: 15), "预置录音应出现在列表")
        let windowBefore = app.windows["main"].frame
        let brandBefore = element("sidebar.brand").frame
        let headerBefore = element("library.pageHeader").frame
        row.click()
        XCTAssertTrue(element("library.player.playButton").waitForExistence(timeout: 10))

        let windowAfter = app.windows["main"].frame
        let brandAfter = element("sidebar.brand").frame
        let headerAfter = element("library.pageHeader").frame
        XCTAssertEqual(windowAfter.minY, windowBefore.minY, accuracy: 1, "主窗口不应上移")
        XCTAssertEqual(windowAfter.height, windowBefore.height, accuracy: 1, "主窗口高度不应被详情撑大")
        XCTAssertEqual(brandAfter.minY, brandBefore.minY, accuracy: 1, "侧栏品牌区不应上移")
        XCTAssertEqual(headerAfter.minY, headerBefore.minY, accuracy: 1, "录音库页头不应上移")
        XCTAssertTrue(element("sidebar.settingsButton").isHittable, "设置按钮应始终可见可点击")
    }

    // MARK: - 7. 开发者模式门控控制台

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
