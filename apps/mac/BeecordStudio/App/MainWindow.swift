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

    /// 窗口档位：UITest（-uiTestWindow）用 CI 档——min 宽与产品最小宽对齐（1024，
    /// 恰等于 CI runner 可视宽，见 TESTING.md §4"CI 环境三条认知"）；早期 900 min 会让
    /// 库页详情列只剩 ~350，操作行/播放器区固有宽 ~460 撑破窗口右缘（最右按钮 AX frame
    /// 落窗外点击被丢，CI run 39040395432 实录）。窄于 1024 的屏幕库页不支持
    /// （详情区多区块固有宽 ~460，断两行仅收缩操作行、救不了整体溢出）。
    fileprivate static var windowFit: WindowFit {
        if LaunchOptions.uiTestWindow {
            return WindowFit(minWidth: 1024, idealWidth: 1140, minHeight: 560, idealHeight: 680)
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
