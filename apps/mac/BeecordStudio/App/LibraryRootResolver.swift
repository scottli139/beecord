import Foundation

/// 录音库根目录解析（Device/Library 两 VM 共用）：
/// `-libraryRoot` 启动参数 > 安全书签目录（会话级持有访问权）> 默认容器。
/// 书签 stale/失效 → 回退默认容器 + toast。
enum LibraryRootResolver {

    @MainActor
    static func resolve(settings: SettingsModel, appModel: AppModel) -> URL {
        if let override = LaunchOptions.libraryRoot {
            return URL(fileURLWithPath: override, isDirectory: true)
        }
        if let data = settings.downloadDirBookmark {
            if let url = SecurityScopedBookmark.resolve(data) {
                _ = url.startAccessingSecurityScopedResource()
                return url
            }
            appModel.showToast(ToastMessage(
                kind: .error,
                text: "下载目录书签已失效，已回退默认目录，请在设置中重新选择"
            ))
        }
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        return base.appendingPathComponent("Beecord", isDirectory: true)
    }
}
