import Foundation

/// Studio 应用模型：侧栏当前页 + Toast 状态。
@MainActor
@Observable
final class AppModel {

    /// 侧栏页面（设备 / 录音库 / 控制台）。
    enum Page: String, CaseIterable, Identifiable {
        case device
        case library
        case console

        var id: String { rawValue }
    }

    /// 当前选中页。
    private(set) var selectedPage: Page = .device
    /// 当前 Toast；nil = 不展示。
    private(set) var toast: ToastMessage?
    /// 同步完成代数（LibraryViewModel 据此刷新 catalog；0 = 从未同步）
    private(set) var syncGeneration = 0

    private var toastTask: Task<Void, Never>?

    // MARK: - 意图方法

    func selectPage(_ page: Page) {
        selectedPage = page
    }

    /// 展示 Toast，按类型自动消失（信息 2s，错误 3s）。
    func showToast(_ message: ToastMessage) {
        toast = message
        toastTask?.cancel()
        let duration = message.kind == .error ? 3.0 : 2.0
        toastTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(duration))
            guard let self, !Task.isCancelled else { return }
            self.toast = nil
        }
    }

    /// 标记一轮同步完成（设备页 VM 在 syncCompleted 事件时调用）。
    func markSyncCompleted() {
        syncGeneration &+= 1
    }

    func dismissToast() {
        toastTask?.cancel()
        toast = nil
    }
}
