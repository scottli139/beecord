import SwiftUI

/// 主窗口：NavigationSplitView（侧栏 216，180~240 可调）+ 详情路由 + Toast 覆盖层。
struct MainWindow: View {
    @Bindable var appModel: AppModel
    let settingsModel: SettingsModel

    var body: some View {
        NavigationSplitView {
            SidebarView(appModel: appModel, settingsModel: settingsModel)
        } detail: {
            detailView
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

    @ViewBuilder
    private var detailView: some View {
        switch appModel.selectedPage {
        case .device:
            DeviceView()
        case .library:
            LibraryView()
        case .console:
            ConsoleView()
        }
    }
}

#Preview("浅色") {
    MainWindow(appModel: AppModel(), settingsModel: SettingsModel())
}

#Preview("深色") {
    MainWindow(appModel: AppModel(), settingsModel: SettingsModel())
        .preferredColorScheme(.dark)
}
