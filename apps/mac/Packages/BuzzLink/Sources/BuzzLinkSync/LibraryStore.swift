import Foundation

/// 录音库写入层抽象（studio-tech-design §5.2/§5.3：文件系统即数据库）。
///
/// 线程模型：全部方法声明为 async，允许 actor 化实现；调用方（SyncEngine）串行访问。
/// 语义契约：
/// - `.partial` 目录中的文件视为"未完成"，``purgeStalePartials()`` 负责清理（P0 无断点续传，
///   重连后整文件重下）；
/// - 幂等键为设备文件 id（`RecordingMeta.source.fileID`），而非 recording-id（冲突后缀不可推导）。
public protocol LibraryStore: Sendable {

    /// 已完成录音的来源设备文件 id 集合（diff 用）；损坏/缺 meta 的目录跳过。
    func existingSourceIDs() async throws -> Set<String>

    /// 为 fileID 准备 `.partial` 临时文件（清掉同名残留），返回其路径。
    func preparePartial(for fileID: String) async throws -> URL

    /// 原子入库：`.partial` 文件 move 为 `Library/<recording-id>/audio.ogg` 并原子写
    /// `meta.json`（`meta.id` 由本方法按实际分配的 recording-id 覆写）。
    /// - Returns: 分配的 recording-id（= 目录名；冲突时带 `-2`/`-3` 短后缀）
    func commitPartial(from partial: URL, meta: RecordingMeta) async throws -> String

    /// 丢弃 fileID 对应的 `.partial` 临时文件（不存在则无事发生）。
    func discardPartial(for fileID: String) async throws

    /// 清理全部 `.partial` 残留（启动时调用；存在即未完成）。
    func purgeStalePartials() async throws
}

/// 录音库错误
public enum LibraryStoreError: Error, LocalizedError, Sendable, Equatable {
    /// 写入失败（携带描述）
    case writeFailed(String)

    public var errorDescription: String? {
        switch self {
        case .writeFailed(let message):
            return "录音库写入失败：\(message)"
        }
    }
}
