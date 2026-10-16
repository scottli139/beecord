import BuzzLink
import Foundation

/// 下载进度记录器（跨并发回调线程安全）：客户端 progress 回调为 `@Sendable`
/// 非隔离，测试用它收集后统一断言（末帧值/单调性/attempt 标记等）。
public final class ClientProgressRecorder: @unchecked Sendable {

    private let lock = NSLock()
    private var storage: [BuzzLinkDownloadProgress] = []

    public init() {}

    /// 已记录进度（加锁快照）
    public var values: [BuzzLinkDownloadProgress] {
        lock.withLock { storage }
    }

    public func record(_ progress: BuzzLinkDownloadProgress) {
        lock.withLock { storage.append(progress) }
    }
}
