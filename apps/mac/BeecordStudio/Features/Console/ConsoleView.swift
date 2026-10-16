import SwiftUI

/// 协议控制台页占位（T4 骨架；侧栏入口受开发者模式门控）。
/// T7 锚点（id 清单，届时在本文件实现）：
/// - 日志表：`console.logTable`（行点击展开 hex）
/// - 过滤：`console.filterField` / `console.directionFilter`
/// - 导出：`console.exportButton`（会话向量导出，TESTING.md §3）
struct ConsoleView: View {
    var body: some View {
        VStack(spacing: 0) {
            pageHeader
            EmptyStateView(
                systemImage: "terminal",
                title: "协议控制台",
                message: "连接设备后，此处将展示 BuzzLink 帧日志与向量导出（T7 上线）。",
                accessibilityID: "console.emptyState"
            )
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    private var pageHeader: some View {
        HStack {
            Text("控制台")
                .font(DesignTokens.FontToken.pageTitle)
            Spacer()
        }
        .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
        .padding(.vertical, DesignTokens.Layout.mainPaddingVertical)
    }
}

#Preview("浅色") {
    ConsoleView()
}

#Preview("深色") {
    ConsoleView()
        .preferredColorScheme(.dark)
}
