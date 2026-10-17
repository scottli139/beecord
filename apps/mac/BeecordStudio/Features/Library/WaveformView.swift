import BuzzLinkAudio
import SwiftUI

/// 波形图（prototype v0.3）：Canvas 绘制 min/max 双值柱——已播 accent、未播 35% accent。
/// R5：整图包 accessibilityElement，AXValue=播放进度 0-1（字符串数值），label 含总时长。
struct WaveformView: View {
    let columns: [WaveformExtractor.Column]
    /// 播放进度 0-1
    var progress: Double
    let durationText: String
    let onSeek: (Double) -> Void

    var body: some View {
        GeometryReader { geometry in
            Canvas { context, size in
                guard !columns.isEmpty else { return }
                let columnCount = CGFloat(columns.count)
                let columnWidth = size.width / columnCount
                let midY = size.height / 2
                for (index, column) in columns.enumerated() {
                    let played = Double(index) / Double(columns.count) <= progress
                    let color: Color = played ? Color.accentColor : Color.accentColor.opacity(0.35)
                    let up = CGFloat(max(0.03, column.max)) * midY
                    let down = CGFloat(max(0.03, abs(min(0, column.min)))) * midY
                    let rect = CGRect(
                        x: CGFloat(index) * columnWidth + columnWidth * 0.2,
                        y: midY - up,
                        width: max(1, columnWidth * 0.6),
                        height: max(1, up + down)
                    )
                    context.fill(Path(rect), with: .color(color))
                }
            }
            .contentShape(Rectangle())
            .onTapGesture { location in
                guard geometry.size.width > 0 else { return }
                onSeek(min(max(0, location.x / geometry.size.width), 1))
            }
        }
        .frame(height: 80) // DESIGN_SYSTEM §2：波形 h80
        .accessibilityElement()
        .accessibilityLabel("波形，总时长 \(durationText)")
        .accessibilityValue(String(format: "%.3f", progress))
        .accessibilityIdentifier("library.waveform")
    }
}
