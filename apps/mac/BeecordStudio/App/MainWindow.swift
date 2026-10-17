import SwiftUI

/// 主窗口：NavigationSplitView（侧栏 216，180~240 可调）+ 详情路由 + Toast 覆盖层。
struct MainWindow: View {
    @Bindable var appModel: AppModel
    let settingsModel: SettingsModel
    let deviceViewModel: DeviceViewModel
    let libraryViewModel: LibraryViewModel

    var body: some View {
        NavigationSplitView {
            SidebarView(appModel: appModel, settingsModel: settingsModel, deviceViewModel: deviceViewModel)
        } detail: {
            switch appModel.selectedPage {
            case .device:
                DeviceView(viewModel: deviceViewModel)
            case .library:
                LibraryView(viewModel: libraryViewModel, appModel: appModel)
            case .console:
                ConsoleView()
            }
        }
        .frame(
            minWidth: Self.windowFit.minWidth,
            idealWidth: Self.windowFit.idealWidth,
            minHeight: Self.windowFit.minHeight,
            idealHeight: Self.windowFit.idealHeight
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

extension MainWindow {
    struct WindowFit: Sendable {
        let minWidth: CGFloat
        let idealWidth: CGFloat
        let minHeight: CGFloat
        let idealHeight: CGFloat
    }

    /// 窗口档位：UITest（-uiTestWindow）用小档保证任何 CI 可视区完整可交互；
    /// 常规走产品值 + 按屏收缩。
    fileprivate static var windowFit: WindowFit {
        if LaunchOptions.uiTestWindow {
            return WindowFit(minWidth: 900, idealWidth: 1080, minHeight: 560, idealHeight: 680)
        }
        return WindowFit(
            minWidth: DesignTokens.Layout.windowMinWidth,
            idealWidth: fittedIdealWidth,
            minHeight: DesignTokens.Layout.windowMinHeight,
            idealHeight: fittedIdealHeight
        )
    }

    /// 理想窗口尺寸按主屏可视区收缩（CI runner 早期小屏下防窗口超高裁底部控件；
    /// 产品最小/理想值不变，正常屏（≥1290×810）无效果）。
    fileprivate static var fittedIdealWidth: CGFloat {
        let screen = NSScreen.main?.visibleFrame.width ?? .greatestFiniteMagnitude
        return min(DesignTokens.Layout.windowIdealWidth, max(DesignTokens.Layout.windowMinWidth, screen - 10))
    }

    fileprivate static var fittedIdealHeight: CGFloat {
        let screen = NSScreen.main?.visibleFrame.height ?? .greatestFiniteMagnitude
        return min(DesignTokens.Layout.windowIdealHeight, max(DesignTokens.Layout.windowMinHeight, screen - 10))
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
        deviceViewModel: DeviceViewModel(appModel: appModel, settings: settingsModel),
        libraryViewModel: LibraryViewModel(appModel: appModel, settings: settingsModel)
    )
}
