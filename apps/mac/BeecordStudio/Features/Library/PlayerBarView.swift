import SwiftUI

/// 播放走带（prototype v0.3）：±15s / 播放暂停 / 倍速循环 / 进度 / 时间（等宽数字）。
/// 全控件 `.fixedSize()` 防截断（评审修复：窄详情区下被压成 "-1…"/"0:03…"）。
struct PlayerBarView: View {
    let isPlaying: Bool
    let fraction: Double
    let positionText: String
    let durationText: String
    let rate: Float
    let onTogglePlay: () -> Void
    let onSkipBack: () -> Void
    let onSkipForward: () -> Void
    let onCycleRate: () -> Void
    let onSeek: (Double) -> Void

    var body: some View {
        HStack(spacing: DesignTokens.Space.s3) {  // 原型 14px → 就近刻度 12
            Button("−15s", action: onSkipBack)
                .fixedSize()
                .accessibilityIdentifier("library.player.back15Button")
            Button(isPlaying ? "暂停" : "播放", action: onTogglePlay)
                .buttonStyle(.borderedProminent)
                .frame(width: 64)
                .accessibilityIdentifier("library.player.playButton")
            Button("+15s", action: onSkipForward)
                .fixedSize()
                .accessibilityIdentifier("library.player.fwd15Button")
            Button(String(format: "%.1f×", rate), action: onCycleRate)
                .fixedSize()
                .accessibilityIdentifier("library.player.speedButton")
            Slider(
                value: Binding(
                    get: { fraction },
                    set: { onSeek($0) }
                ),
                in: 0...1
            )
            .frame(minWidth: 80)
            .accessibilityIdentifier("library.player.progressSlider")
            Text("\(positionText) / \(durationText)")
                .font(DesignTokens.FontToken.digit)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                .fixedSize()
                .accessibilityIdentifier("library.player.timeLabel")
        }
    }
}
