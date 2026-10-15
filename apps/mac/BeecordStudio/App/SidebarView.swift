import SwiftUI

/// 侧栏导航（DESIGN_SYSTEM §2：h28、r6；active 为 accent 15% 底 + accent 字）。
/// 第三项"控制台"受开发者模式门控，默认隐藏。
struct SidebarView: View {
    @Bindable var appModel: AppModel
    let settingsModel: SettingsModel

    var body: some View {
        VStack(spacing: DesignTokens.Space.s2) {
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
        }
        .padding(DesignTokens.Space.s2)
        .navigationSplitViewColumnWidth(
            min: DesignTokens.Layout.sidebarMin,
            ideal: DesignTokens.Layout.sidebarIdeal,
            max: DesignTokens.Layout.sidebarMax
        )
    }
}

/// 单个侧栏导航项。
private struct SidebarItem: View {
    let icon: String
    let title: String
    let page: AppModel.Page
    let accessibilityID: String
    @Bindable var appModel: AppModel

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
        .background(isActive ? Color.accentColor.opacity(0.15) : .clear, in: RoundedRectangle(cornerRadius: DesignTokens.Radius.sm))
        .foregroundStyle(isActive ? Color.accentColor : DesignTokens.ColorToken.textPrimary)
        .accessibilityIdentifier(accessibilityID)
    }
}

#Preview("默认") {
    NavigationSplitView {
        SidebarView(appModel: AppModel(), settingsModel: SettingsModel())
    } detail: {
        DeviceView()
    }
}

#Preview("开发者模式 · 深色") {
    NavigationSplitView {
        SidebarView(appModel: AppModel(), settingsModel: previewSettings(developerMode: true))
    } detail: {
        DeviceView()
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
