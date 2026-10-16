import SwiftUI

/// 主窗口：NavigationSplitView（侧栏 216，180~240 可调）+ 详情路由 + Toast 覆盖层。
struct MainWindow: View {
    @Bindable var appModel: AppModel
    let settingsModel: SettingsModel
    let deviceViewModel: DeviceViewModel

    var body: some View {
        NavigationSplitView {
            SidebarView(appModel: appModel, settingsModel: settingsModel, deviceViewModel: deviceViewModel)
        } detail: {
            switch appModel.selectedPage {
            case .device:
                DeviceView(viewModel: deviceViewModel)
            case .library:
                LibraryView()
            case .console:
                ConsoleView()
            }
        }
        .frame(
            minWidth: DesignTokens.Layout.windowMinWidth,
            idealWidth: DesignTokens.Layout.windowIdealWidth,
            minHeight: DesignTokens.Layout.windowMinHeight,
            idealHeight: DesignTokens.Layout.windowIdealHeight
        )
        .overlay(alignment: .bottom) {
            if let toast = appModel.toast {
                ToastView(message: toast)
                    .padding(.bottom, DesignTokens.Space.s6)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
        .animation(.easeOut(duration: DesignTokens.Motion.fast), value: appModel.toast)
    }
}

#Preview("浅色") {
    previewMainWindow()
}

#Preview("深色") {
    previewMainWindow()
        .preferredColorScheme(.dark)
}

/// 预览用主窗口（新建真实 VM，idle 态无害）。
@MainActor
private func previewMainWindow() -> some View {
    let appModel = AppModel()
    let settingsModel = SettingsModel()
    return MainWindow(
        appModel: appModel,
        settingsModel: settingsModel,
        deviceViewModel: DeviceViewModel(appModel: appModel, settings: settingsModel)
    )
}
