import Foundation

/// 安全书签最小实现（T5 问题 4）：选目录时 `.withSecurityScope` 生成书签存
/// UserDefaults；解析侧由上层持有 `startAccessingSecurityScopedResource()`
/// （会话级）。stale 或解析失败一律返回 nil，由上层回退默认库根并提示。
enum SecurityScopedBookmark {

    /// 生成安全书签；失败返回 nil。
    static func makeBookmark(for url: URL) -> Data? {
        try? url.bookmarkData(options: .withSecurityScope, includingResourceValuesForKeys: nil, relativeTo: nil)
    }

    /// 解析安全书签；stale/损坏/失败返回 nil。
    static func resolve(_ data: Data) -> URL? {
        var isStale = false
        guard let url = try? URL(
            resolvingBookmarkData: data,
            options: .withSecurityScope,
            relativeTo: nil,
            bookmarkDataIsStale: &isStale
        ), !isStale else {
            return nil
        }
        return url
    }
}
