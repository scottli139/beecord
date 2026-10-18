import BuzzLinkAudio
import SwiftUI

/// 波形图（prototype v0.3 观感：粗条可数）：Canvas 渲染——数据层 600 列经
/// ``WaveformBinner`` 按实际宽度聚合（条宽 3pt + 间隙 2pt），已播 accent、未播 35% accent。
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
                let spec = WaveformBinner.barSpec(width: size.width)
                let bars = WaveformBinner.aggregate(columns, targetCount: spec.count)
                guard !bars.isEmpty else { return }
                let midY = size.height / 2
                for (index, column) in bars.enumerated() {
                    let played = Double(index) / Double(bars.count) <= progress
                    let color: Color = played ? Color.accentColor : Color.accentColor.opacity(0.35)
                    let up = CGFloat(max(0.03, column.max)) * midY
                    let down = CGFloat(max(0.03, abs(min(0, column.min)))) * midY
                    let x = CGFloat(index) * (spec.barWidth + spec.gap)
                    let rect = CGRect(
                        x: x,
                        y: midY - up,
                        width: spec.barWidth,
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
        .accessibilityAddTraits(.isImage)
        .accessibilityLabel("波形，总时长 \(durationText)")
        .accessibilityValue(String(format: "%.3f", progress))
        .accessibilityIdentifier("library.waveform")
    }
}
