import Foundation

/// 启动参数解析（阶段 D 的 UITest 注入点；本阶段只做实现）。
///
/// - `-resetSettings`：启动早期清空全部 `settings.*` UserDefaults 键；
/// - `-libraryRoot <path>`：覆盖录音库根目录（优先于书签目录与默认容器）。
enum LaunchOptions {

    private static let arguments = ProcessInfo.processInfo.arguments

    static let resetSettings = arguments.contains("-resetSettings")

    /// -autoSyncOff：重置后把 settings.autoSyncEnabled 置 false（设备流 UITest
    /// 手动下载用例的注入点；须与 -resetSettings 同用，避免污染用户设置）。
    static let autoSyncOff = arguments.contains("-autoSyncOff")

    /// -uiTestWindow：UITest 专用窗口档位（min 900×560 / ideal 1080×680）。
    /// CI runner 早期屏小/Dock 占位下，产品档（min 1140 / ideal 1280×800）会把
    /// 底部控件挤出可视区（hittable 失败）或触发侧栏折叠；测试档保证
    /// 任何 ≥1100×740 可视区完整可交互。产品值不受影响。
    static let uiTestWindow = arguments.contains("-uiTestWindow")

    static let libraryRoot: String? = value(for: "-libraryRoot")

    /// App 启动早期调用（须早于 SettingsModel 初始化）。
    static func applyIfNeeded() {
        guard resetSettings else { return }
        let defaults = UserDefaults.standard
        for key in defaults.dictionaryRepresentation().keys where key.hasPrefix("settings.") {
            defaults.removeObject(forKey: key)
        }
        if autoSyncOff {
            defaults.set(false, forKey: "settings.autoSyncEnabled")
        }
    }

    /// 取参名的下一个参数值；不存在或越界返回 nil。
    private static func value(for name: String) -> String? {
        guard let index = arguments.firstIndex(of: name) else { return nil }
        let nextIndex = arguments.index(after: index)
        guard nextIndex < arguments.count else { return nil }
        let candidate = arguments[nextIndex]
        return candidate.hasPrefix("-") ? nil : candidate
    }
}
