import Foundation

/// UITest 启动链路探针：仅当注入 `-libraryRoot`（即 UITest 启动）时激活，
/// 追加带 systemUptime 的事件行到 `<libraryRoot>/uitest-launch-trace.log`。
/// 用例失败时由测试进程回读并进失败信息——CI 无 xcresult 时定位启动卡点的唯一手段。
enum LaunchTrace {

    static func mark(_ event: String) {
        guard let root = LaunchOptions.libraryRoot else { return }
        let url = URL(fileURLWithPath: root, isDirectory: true)
            .appendingPathComponent("uitest-launch-trace.log")
        let line = String(format: "%.3f %@\n", ProcessInfo.processInfo.systemUptime, event)
        guard let data = line.data(using: .utf8) else { return }
        if let handle = try? FileHandle(forWritingTo: url) {
            defer { try? handle.close() }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: data)
        } else {
            try? data.write(to: url, options: .atomic)
        }
    }
}
