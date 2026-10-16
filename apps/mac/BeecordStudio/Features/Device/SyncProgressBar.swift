import BuzzLinkSync
import SwiftUI

/// 底部整体同步进度条（ux §3.2：x/y 个、当前文件 %、速率 KB/s、ETA、【取消】）。
/// `device.downloadProgress` 暴露 AXValue 0-1（R4）。
struct SyncProgressBar: View {
    let progress: SyncProgress
    let isCancelling: Bool
    let onCancel: () -> Void

    var body: some View {
        HStack(spacing: DesignTokens.Space.s3) {
            ProgressView(value: progress.overallFraction)
                .progressViewStyle(.linear)
                .frame(width: 160)
                .accessibilityIdentifier("device.downloadProgress")
            Text(progressText)
                .font(DesignTokens.FontToken.digit)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                .lineLimit(1)
                .accessibilityIdentifier("device.progressText")
            Spacer()
            if isCancelling {
                Text("正在取消…")
                    .font(DesignTokens.FontToken.callout)
                    .foregroundStyle(DesignTokens.ColorToken.textSecondary)
            } else {
                Button("取消") {
                    onCancel()
                }
                .accessibilityIdentifier("device.cancelButton")
            }
        }
        .padding(.horizontal, DesignTokens.Space.s4)
        .padding(.vertical, DesignTokens.Space.s2)
        .background(DesignTokens.ColorToken.surfaceCard2, in: RoundedRectangle(cornerRadius: DesignTokens.Radius.sm))
    }

    /// 聚合描述：x/y 个 · 当前文件 % · 速率 · ETA。
    private var progressText: String {
        var parts: [String] = ["\(progress.completedFiles)/\(progress.totalFiles) 个"]
        if let current = progress.currentFileID {
            parts.append("\(current) \(Int((progress.fileFraction * 100).rounded()))%")
        }
        let kbps = progress.bytesPerSecond / 1024
        parts.append(String(format: "%.0f KB/s", kbps))
        if let eta = progress.etaSeconds {
            parts.append("预计 \(Self.etaText(eta))")
        }
        return parts.joined(separator: " · ")
    }

    static func etaText(_ seconds: TimeInterval) -> String {
        let total = Int(seconds.rounded())
        if total < 60 { return "\(max(1, total)) 秒" }
        return "\(total / 60) 分钟"
    }
}

#Preview("传输中") {
    SyncProgressBar(
        progress: SyncProgress(
            completedFiles: 1,
            totalFiles: 3,
            currentFileID: "REC_20260102_093000",
            fileFraction: 0.42,
            overallFraction: 0.5,
            bytesPerSecond: 24 * 1024,
            etaSeconds: 75
        ),
        isCancelling: false,
        onCancel: {}
    )
    .padding()
}

#Preview("取消中 · 深色") {
    SyncProgressBar(
        progress: SyncProgress(
            completedFiles: 0,
            totalFiles: 3,
            currentFileID: "REC_20260102_093000",
            fileFraction: 0.1,
            overallFraction: 0.03,
            bytesPerSecond: 0,
            etaSeconds: nil
        ),
        isCancelling: true,
        onCancel: {}
    )
    .padding()
    .preferredColorScheme(.dark)
}
