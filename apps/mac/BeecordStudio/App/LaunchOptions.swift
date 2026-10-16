import Foundation

/// 启动参数解析（阶段 D 的 UITest 注入点；本阶段只做实现）。
///
/// - `-resetSettings`：启动早期清空全部 `settings.*` UserDefaults 键；
/// - `-libraryRoot <path>`：覆盖录音库根目录（优先于书签目录与默认容器）。
enum LaunchOptions {

    private static let arguments = ProcessInfo.processInfo.arguments

    static let resetSettings = arguments.contains("-resetSettings")

    static let libraryRoot: String? = value(for: "-libraryRoot")

    /// App 启动早期调用（须早于 SettingsModel 初始化）。
    static func applyIfNeeded() {
        guard resetSettings else { return }
        let defaults = UserDefaults.standard
        for key in defaults.dictionaryRepresentation().keys where key.hasPrefix("settings.") {
            defaults.removeObject(forKey: key)
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
