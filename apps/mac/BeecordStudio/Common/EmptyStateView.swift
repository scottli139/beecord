import SwiftUI

/// 通用空态组件（DESIGN_SYSTEM §2：图标 40 + 标题 15/600 + 说明 13）。
struct EmptyStateView: View {
    let systemImage: String
    let title: String
    let message: String
    /// 容器 AX id（R1：`{page}.emptyState`）。
    let accessibilityID: String

    var body: some View {
        VStack(spacing: DesignTokens.Space.s3) {
            Image(systemName: systemImage)
                .font(.system(size: 40))
                .foregroundStyle(DesignTokens.ColorToken.textTertiary)
            Text(title)
                .font(DesignTokens.FontToken.emptyStateTitle)
            Text(message)
                .font(DesignTokens.FontToken.body)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                .multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(DesignTokens.Space.s8)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier(accessibilityID)
    }
}

#Preview("浅色") {
    EmptyStateView(
        systemImage: "applewatch",
        title: "尚未连接设备",
        message: "打开 Beecord 设备并进入同步模式后，即可扫描连接。",
        accessibilityID: "device.emptyState"
    )
}

#Preview("深色") {
    EmptyStateView(
        systemImage: "terminal",
        title: "协议控制台",
        message: "连接设备后，此处将展示 BuzzLink 帧日志。",
        accessibilityID: "console.emptyState"
    )
    .preferredColorScheme(.dark)
}
