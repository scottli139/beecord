import BuzzLinkSync
import SwiftUI

/// 设备文件表（ux §3.2：勾选 / 文件名 / 时长 / 大小 / 状态徽标；失败行内【重试】）。
/// 行高 28；行锚点用稳定业务 id（R2：`device.fileTable.row.{fileID}`）。
struct FileTableView: View {
    let rows: [DeviceFileRow]
    let selectedIDs: Set<String>
    let onToggle: (String) -> Void
    let onRetry: (String) -> Void

    var body: some View {
        ScrollView {
            LazyVStack(spacing: 0) {
                ForEach(rows) { row in
                    FileRowView(
                        row: row,
                        isSelected: selectedIDs.contains(row.id),
                        onToggle: { onToggle(row.id) },
                        onRetry: { onRetry(row.id) }
                    )
                    Divider()
                }
            }
        }
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("device.fileTable")
    }

    static func durationText(_ ms: UInt32) -> String {
        let totalSeconds = Int(ms) / 1_000
        return String(format: "%d:%02d", totalSeconds / 60, totalSeconds % 60)
    }

    static func sizeText(_ bytes: Int) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .binary)
    }
}

/// 单行：checkbox（真按钮，R6）+ 文件名 + 时长 + 大小 + 状态徽标（R4 可读文本）。
private struct FileRowView: View {
    let row: DeviceFileRow
    let isSelected: Bool
    let onToggle: () -> Void
    let onRetry: () -> Void

    var body: some View {
        HStack(spacing: DesignTokens.Space.s2) {
            Button(action: onToggle) {
                Image(systemName: isSelected ? "checkmark.square.fill" : "square")
                    .font(.system(size: 14))
                    .frame(width: 20, height: 20)
            }
            .buttonStyle(.plain)
            .foregroundStyle(isSelected ? Color.accentColor : DesignTokens.ColorToken.textSecondary)
            .accessibilityLabel("选择 \(row.id)")
            .accessibilityIdentifier("device.fileTable.row.\(row.id).checkbox")
            Text(row.id)
                .font(DesignTokens.FontToken.body)
                .lineLimit(1)
                .truncationMode(.middle)
                .frame(maxWidth: .infinity, alignment: .leading)
            Text(FileTableView.durationText(row.durationMs))
                .font(DesignTokens.FontToken.digit)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                .frame(width: 56, alignment: .trailing)
            Text(FileTableView.sizeText(row.size))
                .font(DesignTokens.FontToken.digit)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                .frame(width: 72, alignment: .trailing)
            statusBadge
                .frame(width: 112, alignment: .trailing)
        }
        .frame(height: 28)
        .padding(.horizontal, DesignTokens.Space.s2)
        .background(isSelected ? Color.accentColor.opacity(0.06) : Color.clear)
        .contentShape(Rectangle())
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("device.fileTable.row.\(row.id)")
    }

    @ViewBuilder
    private var statusBadge: some View {
        switch row.status {
        case .notDownloaded:
            Text("未下载")
                .font(DesignTokens.FontToken.callout)
                .foregroundStyle(DesignTokens.ColorToken.textTertiary)
                .accessibilityIdentifier("device.fileTable.row.\(row.id).status")
        case .downloading(let fraction):
            Text("下载中 \(Int((fraction * 100).rounded()))%")
                .font(DesignTokens.FontToken.digit)
                .foregroundStyle(Color.accentColor)
                .accessibilityIdentifier("device.fileTable.row.\(row.id).status")
        case .verified:
            Text("✓ 已校验")
                .font(DesignTokens.FontToken.callout)
                .foregroundStyle(DesignTokens.ColorToken.stateSuccess)
                .accessibilityIdentifier("device.fileTable.row.\(row.id).status")
        case .failed:
            HStack(spacing: DesignTokens.Space.s2) {
                Text("失败")
                    .font(DesignTokens.FontToken.callout)
                    .foregroundStyle(DesignTokens.ColorToken.stateDanger)
                    .accessibilityIdentifier("device.fileTable.row.\(row.id).status")
                Button("重试") {
                    onRetry()
                }
                .font(DesignTokens.FontToken.callout)
                .accessibilityIdentifier("device.fileTable.row.\(row.id).retryButton")
            }
        case .incomplete:
            Text("未完成")
                .font(DesignTokens.FontToken.callout)
                .foregroundStyle(DesignTokens.ColorToken.stateWarning)
                .accessibilityIdentifier("device.fileTable.row.\(row.id).status")
        }
    }
}

#Preview("浅色") {
    FileTableView(
        rows: [
            DeviceFileRow(id: "REC_20260103_074500", durationMs: 120_006, size: 660_000, status: .notDownloaded),
            DeviceFileRow(id: "REC_20260102_093000", durationMs: 30_006, size: 165_000, status: .downloading(fraction: 0.42)),
            DeviceFileRow(id: "REC_20260101_120000", durationMs: 5_006, size: 28_000, status: .verified),
            DeviceFileRow(id: "REC_20251231_235900", durationMs: 8_000, size: 44_000, status: .failed("CRC 校验失败")),
            DeviceFileRow(id: "REC_20251230_080000", durationMs: 60_000, size: 330_000, status: .incomplete)
        ],
        selectedIDs: ["REC_20260103_074500"],
        onToggle: { _ in },
        onRetry: { _ in }
    )
    .frame(height: 220)
    .padding()
}

#Preview("深色") {
    FileTableView(
        rows: [
            DeviceFileRow(id: "REC_20260101_120000", durationMs: 5_006, size: 28_000, status: .verified)
        ],
        selectedIDs: [],
        onToggle: { _ in },
        onRetry: { _ in }
    )
    .frame(height: 60)
    .padding()
    .preferredColorScheme(.dark)
}
