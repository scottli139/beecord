import Foundation

/// 滑动窗口速率估算：累计字节时间序列在窗口内的平均速率。
/// 样本须时间单调递增（字节数允许因重试趟回落）；新队列开始即丢弃旧实例。
public struct RateEstimator: Sendable {

    /// 窗口长度（秒）
    public let windowSeconds: TimeInterval
    private var samples: [(t: TimeInterval, cumulativeBytes: Int)] = []

    public init(windowSeconds: TimeInterval = 5) {
        self.windowSeconds = windowSeconds
    }

    /// 记录一个样本（t 为单调秒）
    public mutating func addSample(t: TimeInterval, cumulativeBytes: Int) {
        samples.append((t: t, cumulativeBytes: cumulativeBytes))
        let cutoff = t - windowSeconds
        while samples.count > 1, samples[0].t < cutoff {
            samples.removeFirst()
        }
    }

    /// 窗口内平均速率（字节/秒）；样本不足 2 个或时间跨度为 0 时为 0
    public var bytesPerSecond: Double {
        guard samples.count >= 2, let first = samples.first, let last = samples.last else { return 0 }
        let span = last.t - first.t
        guard span > 0 else { return 0 }
        return Double(last.cumulativeBytes - first.cumulativeBytes) / span
    }
}
