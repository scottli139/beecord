import Foundation

/// 波形渲染层聚合（T6 评审拍板）：数据层 600 列不动，渲染按实际宽度自适应聚合为
/// 目标条数——条宽 3pt + 间隙 2pt（原型 72 根粗条观感；704pt ≈141 根、564pt ≈113 根）。
/// 聚合规则：每组取 min 的最小值 / max 的最大值（保持 min/max 语义）。
public enum WaveformBinner {

    /// 单列渲染几何（pt）
    public struct BarSpec: Sendable, Equatable {
        /// 条宽
        public var barWidth: CGFloat
        /// 实际间隙（含余量均分）
        public var gap: CGFloat
        /// 条数
        public var count: Int
    }

    /// 按画布宽度计算条规格：条宽 3pt、间隙 2pt；宽度不足整数根时余量均分到间隙。
    /// 极端窄（<200pt）降级条宽/间隙 1pt 并减根数，min 1 根不崩。
    public static func barSpec(width: CGFloat) -> BarSpec {
        let baseBar: CGFloat = 3
        let baseGap: CGFloat = 2
        guard width > 0 else { return BarSpec(barWidth: max(1, baseBar), gap: baseGap, count: 1) }

        if width < 200 {
            let bar: CGFloat = 1
            var gap: CGFloat = 1
            var count = max(1, Int((width + gap) / (bar + gap)))
            while count > 1, CGFloat(count) * bar + CGFloat(count - 1) * gap > width {
                count -= 1
            }
            let used = CGFloat(count) * bar + CGFloat(count - 1) * gap
            if count > 1, used < width { gap += (width - used) / CGFloat(count - 1) }
            return BarSpec(barWidth: bar, gap: gap, count: count)
        }

        var count = max(1, Int((width + baseGap) / (baseBar + baseGap)))
        let used = CGFloat(count) * baseBar + CGFloat(count - 1) * baseGap
        var gap = baseGap
        if count > 1, used < width { gap += (width - used) / CGFloat(count - 1) }  // 余量均分到间隙
        return BarSpec(barWidth: baseBar, gap: gap, count: count)
    }

    /// 聚合：columns（通常为 600 列）→ targetCount 组；末组兜底到尾（防浮点丢列）。
    public static func aggregate(
        _ columns: [WaveformExtractor.Column],
        targetCount: Int
    ) -> [WaveformExtractor.Column] {
        guard !columns.isEmpty, targetCount > 0 else { return [] }
        guard columns.count > targetCount else { return columns }
        var out: [WaveformExtractor.Column] = []
        out.reserveCapacity(targetCount)
        let groupSize = Double(columns.count) / Double(targetCount)
        for index in 0..<targetCount {
            let start = Int(Double(index) * groupSize)
            let end = index == targetCount - 1 ? columns.count : max(start + 1, Int(Double(index + 1) * groupSize))
            var lo = Float.greatestFiniteMagnitude
            var hi = -Float.greatestFiniteMagnitude
            for cursor in start..<max(start + 1, end) where cursor < columns.count {
                lo = min(lo, columns[cursor].min)
                hi = max(hi, columns[cursor].max)
            }
            out.append(WaveformExtractor.Column(min: lo, max: hi))
        }
        return out
    }
}
