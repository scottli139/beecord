import SwiftUI

/// 设备页占位（T4 骨架）。
/// T5 锚点（id 清单，届时在本文件实现）：
/// - 扫描/连接：`device.scanButton` / `device.connectButton` / `device.deviceCard`
/// - 文件表：`device.fileTable.row.{fileName}` / `.status` / `device.selectAllCheckbox`
/// - 下载：`device.downloadButton` / `device.downloadProgress`（AXValue 0-1）/ `device.refreshButton`
struct DeviceView: View {
    var body: some View {
        VStack(spacing: 0) {
            pageHeader
            EmptyStateView(
                systemImage: "applewatch",
                title: "尚未连接设备",
                message: "将 Beecord 设备置于同步模式后，即可扫描并同步录音（T5 上线）。",
                accessibilityID: "device.emptyState"
            )
        }
    }

    private var pageHeader: some View {
        HStack {
            Text("设备")
                .font(DesignTokens.FontToken.pageTitle)
            Spacer()
        }
        .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
        .padding(.vertical, DesignTokens.Layout.mainPaddingVertical)
    }
}

#Preview("浅色") {
    DeviceView()
}

#Preview("深色") {
    DeviceView()
        .preferredColorScheme(.dark)
}
