import SwiftUI

/// 侧栏：品牌头 + 导航（设备/录音库/控制台[开发者模式门控]）+ 底部脚标行（连接状态 + 设置齿轮）。
/// 设置为应用级入口（脚标行齿轮，打开标准 Settings 窗口 ⌘,），不占导航项、无 active 态（原型 v0.3）。
/// AX：`sidebar.brand` / `sidebar.navItem.{devices,library,console}` / `sidebar.deviceStatus` / `sidebar.settingsButton`。
struct SidebarView: View {
    @Bindable var appModel: AppModel
    let settingsModel: SettingsModel
    let deviceViewModel: DeviceViewModel

    @Environment(\.openSettings) private var openSettings

    var body: some View {
        VStack(spacing: DesignTokens.Space.s2) {
            brandHeader
            SidebarItem(
                icon: "applewatch",
                title: "设备",
                page: .device,
                accessibilityID: "sidebar.navItem.devices",
                appModel: appModel
            )
            SidebarItem(
                icon: "waveform",
                title: "录音库",
                page: .library,
                accessibilityID: "sidebar.navItem.library",
                appModel: appModel
            )
            if settingsModel.developerMode {
                SidebarItem(
                    icon: "terminal",
                    title: "控制台",
                    page: .console,
                    accessibilityID: "sidebar.navItem.console",
                    appModel: appModel
                )
            }
            Spacer()
            footerRow
        }
        .padding(DesignTokens.Space.s2)
        .navigationSplitViewColumnWidth(
            min: DesignTokens.Layout.sidebarMin,
            ideal: DesignTokens.Layout.sidebarIdeal,
            max: DesignTokens.Layout.sidebarMax
        )
    }

    /// 品牌头（原型 .brand：icon + app 名，13/600 secondary）
    private var brandHeader: some View {
        HStack(spacing: DesignTokens.Space.s2) {
            Image("BrandMark")
                .resizable()
                .frame(width: 16, height: 16)
            Text("Beecord Studio")
                .font(DesignTokens.FontToken.groupTitle)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
            Spacer()
        }
        .padding(.horizontal, DesignTokens.Space.s2)
        .padding(.bottom, DesignTokens.Space.s3)
        .accessibilityIdentifier("sidebar.brand")
    }

    /// 底部脚标行：连接状态（灰=未连接 / 绿=已连接 / 黄=蓝牙不可用）+ 右侧设置齿轮。
    /// 齿轮打开标准 Settings 窗口（⌘, 同入口）；设置是应用级操作，不占导航项（原型 .dev-status + .dev-gear）。
    private var footerRow: some View {
        HStack(spacing: DesignTokens.Space.s2) {
            Circle()
                .fill(statusDotColor)
                .frame(width: 8, height: 8)
            Text(statusText)
                .font(DesignTokens.FontToken.callout)
                .lineLimit(1)
                .truncationMode(.tail)
            Spacer()
            SettingsGearButton { openSettings() }
        }
        .padding(.horizontal, DesignTokens.Space.s2)
        .padding(.vertical, DesignTokens.Space.s2)
        .background(DesignTokens.ColorToken.surfaceCard2, in: RoundedRectangle(cornerRadius: DesignTokens.Radius.sm))
        .accessibilityIdentifier("sidebar.deviceStatus")
    }

    private var statusText: String {
        if deviceViewModel.permissionOverlay != nil {
            return "蓝牙不可用"
        }
        if deviceViewModel.isConnected {
            let name = deviceViewModel.snapshot.deviceInfo?.deviceName
            return name.map { "\($0) 已连接" } ?? "已连接"
        }
        return "未连接"
    }

    private var statusDotColor: Color {
        if deviceViewModel.permissionOverlay != nil {
            return DesignTokens.ColorToken.stateWarning
        }
        if deviceViewModel.isConnected {
            return DesignTokens.ColorToken.stateSuccess
        }
        return DesignTokens.ColorToken.textTertiary
    }
}

/// 单个侧栏导航项。自绘控件（.plain），hover 态按 DESIGN_SYSTEM §2.2 自补 fill.hover 底。
private struct SidebarItem: View {
    let icon: String
    let title: String
    let page: AppModel.Page
    let accessibilityID: String
    @Bindable var appModel: AppModel
    @State private var isHovering = false

    private var isActive: Bool { appModel.selectedPage == page }

    var body: some View {
        Button {
            appModel.selectPage(page)
        } label: {
            HStack(spacing: DesignTokens.Space.s2) {
                Image(systemName: icon)
                    .font(.system(size: 14))
                    .frame(width: 16)
                Text(title)
                    .font(DesignTokens.FontToken.body)
                Spacer()
            }
            .frame(height: 28)
            .padding(.horizontal, DesignTokens.Space.s2)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .background(
            isActive ? Color.accentColor.opacity(0.15) : (isHovering ? DesignTokens.ColorToken.fillHover : .clear),
            in: RoundedRectangle(cornerRadius: DesignTokens.Radius.sm)
        )
        .foregroundStyle(isActive ? Color.accentColor : DesignTokens.ColorToken.textPrimary)
        .onHover { isHovering = $0 }
        .accessibilityIdentifier(accessibilityID)
    }
}

/// 脚标设置齿轮：自绘控件（.plain），hover 态按 DESIGN_SYSTEM §2.2 自补 fill.hover 底。
private struct SettingsGearButton: View {
    let action: () -> Void
    @State private var isHovering = false

    var body: some View {
        Button(action: action) {
            Image(systemName: "gear")
                .font(.system(size: 13))
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                .frame(width: 22, height: 20)
                .background(
                    isHovering ? DesignTokens.ColorToken.fillHover : .clear,
                    in: RoundedRectangle(cornerRadius: DesignTokens.Radius.sm)
                )
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { isHovering = $0 }
        .accessibilityIdentifier("sidebar.settingsButton")
        .accessibilityLabel("设置")
    }
}

#Preview("默认") {
    let appModel = AppModel()
    let settingsModel = SettingsModel()
    let deviceViewModel = DeviceViewModel(appModel: appModel, settings: settingsModel)
    return NavigationSplitView {
        SidebarView(
            appModel: appModel,
            settingsModel: settingsModel,
            deviceViewModel: deviceViewModel
        )
    } detail: {
        DeviceView(viewModel: deviceViewModel)
    }
}

#Preview("开发者模式 · 深色") {
    let appModel = AppModel()
    let settingsModel = previewSettings(developerMode: true)
    let deviceViewModel = DeviceViewModel(appModel: appModel, settings: settingsModel)
    return NavigationSplitView {
        SidebarView(
            appModel: appModel,
            settingsModel: settingsModel,
            deviceViewModel: deviceViewModel
        )
    } detail: {
        DeviceView(viewModel: deviceViewModel)
    }
    .preferredColorScheme(.dark)
}

/// 预览用模型（开发者模式按参数预设）。
@MainActor
private func previewSettings(developerMode: Bool) -> SettingsModel {
    let model = SettingsModel()
    model.setDeveloperMode(developerMode)
    return model
}
