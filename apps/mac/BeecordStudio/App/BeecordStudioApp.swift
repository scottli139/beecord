import AppKit
import SwiftUI

/// 补窗代理：macOS 26 双端实录（本机 open -a + CI runner）——SwiftUI 场景在无恢复态冷启动
/// （全新用户目录/无 Saved Application State）时不自动建主窗；Window 单例场景下 ⌘N 又无
/// New 命令可兜底（WindowGroup 时代的 ⌘N 兜底对此失效）。这里监听"应用激活"：检测到零窗口
/// 时显式 openWindow 补建单例窗——openWindow 对 Window 场景幂等（已存在则聚焦，不叠窗）。
/// 同时承担尺寸校正：Window 场景忽略 .defaultSize/内容 frame 的 ideal（系统默认 ~1280×789），
/// 且 -uiTestWindow 档位依赖窗口尺寸约束——补窗时对每个新窗口一次性应用档位（幂等标记，
/// 不重置用户手动调整）。UITest 的 ⌘N 兜底保留作第二道防线（对 Window 场景无效果，无害）。
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    var openWindowAction: OpenWindowAction?
    /// 已应用尺寸档的窗口（每窗口只校正一次；新窗口创建后为 nil）
    private weak var sizeCorrectedWindow: NSWindow?

    /// ⌘, 直达设置窗：AppKit 事件监视器（macOS 26.5 实录——SwiftUI 侧
    /// .keyboardShortcut 与 imperative keyEquivalent 在 CommandGroup(replacing:
    /// .appSettings) 上均"显示但事件不派发"，SwiftUI 重渲染还会覆盖命令行补丁；
    /// 监视器在应用事件派发内拦截真实键盘 ⌘, 直接 openWindow；XCTest 的键盘
    /// 注入在本机不到达事件循环，因此 UI 测试统一走应用菜单的 AX 路径）。
    private var settingsKeyMonitor: Any?

    /// 注入 openWindow（App body 每次求值写入；监视器回调经此打开设置窗）。
    func applicationDidFinishLaunching(_ notification: Notification) {
        settingsKeyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) {
            [weak self] event in
            guard let self,
                  event.modifierFlags.contains(.command),
                  event.charactersIgnoringModifiers == ",",
                  !event.isARepeat else { return event }
            self.openWindowAction?(id: "settings")
            return nil // 事件已消费
        }
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        if NSApp.windows.isEmpty {
            openWindowAction?(id: "main")
        }
        applyWindowFitOnce()
    }

    /// 对主窗一次性应用窗口尺寸档（Window 场景系统默认尺寸被忽略档位的补偿；
    /// 用户手动 resize 后不被重置——仅新窗口的首个激活时机执行一次）。
    private func applyWindowFitOnce() {
        guard let window = NSApp.windows.first(where: { $0.identifier?.rawValue == "main" }),
              sizeCorrectedWindow !== window else { return }
        let target = MainWindow.windowFitDefaultSize
        guard window.contentLayoutRect.size.width > 0 else { return }
        if abs(window.contentLayoutRect.width - target.width) > 1
            || abs(window.contentLayoutRect.height - target.height) > 1 {
            window.setContentSize(target)
        }
        sizeCorrectedWindow = window
    }
}

/// Beecord Studio 入口：主窗口（侧栏三页）+ 显式 settings 窗口（Window(id:"settings")，
/// ⌘, 经 CommandGroup(replacing: .appSettings) 打开）+ 页面快捷键 ⌘1/2/3。
/// 注（2026-10-30）：不再依赖自动 Settings 场景——macOS 26.5 实测 Window(id:"main") +
/// Settings{} 组合下 ⌘, 不开设置窗（系统设置窗服务对单窗 App 的派发问题）；
/// 显式 Window + openWindow 后 ⌘, 行为稳定，且 openWindow 对同 id 幂等不叠窗。
@main
struct BeecordStudioApp: App {
    @State private var appModel: AppModel
    @State private var settingsModel: SettingsModel
    @State private var deviceViewModel: DeviceViewModel
    @State private var libraryViewModel: LibraryViewModel
    @State private var consoleViewModel: ConsoleViewModel
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @Environment(\.openWindow) private var openWindow

    init() {
        // 启动参数须早于 SettingsModel 初始化（-resetSettings 清 settings.*）
        LaunchOptions.applyIfNeeded()
        LaunchTrace.mark("app.init")
        let appModel = AppModel()
        let settingsModel = SettingsModel()
        let deviceViewModel = DeviceViewModel(
            appModel: appModel,
            settings: settingsModel
        )
        self._appModel = State(wrappedValue: appModel)
        self._settingsModel = State(wrappedValue: settingsModel)
        self._deviceViewModel = State(wrappedValue: deviceViewModel)
        self._libraryViewModel = State(wrappedValue: LibraryViewModel(
            appModel: appModel,
            settings: settingsModel
        ))
        self._consoleViewModel = State(wrappedValue: ConsoleViewModel(
            appModel: appModel,
            deviceViewModel: deviceViewModel
        ))
    }

    var body: some Scene {
        // 每次求值注入 openWindow 动作（幂等）；无恢复态冷启动时窗口补建靠 AppDelegate。
        let _ = appDelegate.openWindowAction = openWindow
        Window("Beecord Studio", id: "main") {
            MainWindow(
                appModel: appModel,
                settingsModel: settingsModel,
                deviceViewModel: deviceViewModel,
                libraryViewModel: libraryViewModel,
                consoleViewModel: consoleViewModel
            )
        }
        // 窗口尺寸档：macOS 26 的 Window 场景忽略 .defaultSize 与内容 frame 的 ideal
        // （系统默认 ~1280×789），尺寸校正改由 AppDelegate 补窗时一次性应用
        // （MainWindow.windowFitDefaultSize，含屏幕收缩）。
        .commands {
            SidebarCommands()  // 标准侧栏开关（⌃⌘S）：小屏下侧栏折叠时 UITest 可恢复
            // 设置窗：替换系统 .appSettings 槽位，打开动作走显式 Window(id:"settings")。
            // 菜单保留 ⌘, 展示；macOS 26.5 的派发问题由 AppDelegate 监视器兜底。
            CommandGroup(replacing: .appSettings) {
                Button("设置…") {
                    openWindow(id: "settings")
                }
                .keyboardShortcut(",", modifiers: .command)
            }
            CommandMenu("视图") {
                Button("设备") {
                    appModel.selectPage(.device)
                }
                .keyboardShortcut("1", modifiers: .command)
                Button("录音库") {
                    appModel.selectPage(.library)
                }
                .keyboardShortcut("2", modifiers: .command)
                Button("控制台") {
                    appModel.selectPage(.console)
                }
                .keyboardShortcut("3", modifiers: .command)
                .disabled(!settingsModel.developerMode)
            }
        }
        // 显式设置窗（替代自动 Settings 场景；同 id 幂等聚焦，不叠窗）
        Window("设置", id: "settings") {
            SettingsView(settingsModel: settingsModel)
        }
        .windowResizability(.contentSize)
    }
}
