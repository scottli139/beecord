import SwiftUI

/// 录音库页占位（T4 骨架）。
/// T6 锚点（id 清单，届时在本文件实现）：
/// - 录音列表：`library.recordingTable.row.{fileName}`
/// - 详情 Tab：`library.detailTabs` / 播放器 `library.player.playButton` / `.progressSlider`
/// - 操作：`library.exportButton` / `library.deleteButton` / `library.searchField`
struct LibraryView: View {
    var body: some View {
        VStack(spacing: 0) {
            pageHeader
            EmptyStateView(
                systemImage: "waveform",
                title: "录音库为空",
                message: "连接设备并完成同步后，录音将出现在这里（T6 上线）。",
                accessibilityID: "library.emptyState"
            )
        }
    }

    private var pageHeader: some View {
        HStack {
            Text("录音库")
                .font(DesignTokens.FontToken.pageTitle)
            Spacer()
        }
        .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
        .padding(.vertical, DesignTokens.Layout.mainPaddingVertical)
    }
}

#Preview("浅色") {
    LibraryView()
}

#Preview("深色") {
    LibraryView()
        .preferredColorScheme(.dark)
}
