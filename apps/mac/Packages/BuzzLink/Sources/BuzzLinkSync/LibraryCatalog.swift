import Foundation
import os

/// 录音库目录（studio-tech-design §5.3"启动时目录扫描对账 + manifest 缓存"）。
///
/// 线程模型：actor。全量扫描 `Library/*/meta.json` 构建内存目录；manifest 缓存
/// `catalog.json`（`<root>/Library/.catalog.json`）记录各目录 meta 修改时间，
/// 增量刷新跳过未变化项。变更以 ``changes`` 流边沿上报（added/removed/updated），
/// UI 消费流做局部更新而非全量重扫。坏 meta 跳过 + 日志（同 FileLibraryStore 策略）。
public actor LibraryCatalog {

    /// 目录变更事件（边沿语义）
    public enum Change: Sendable, Equatable {
        case added(meta: RecordingMeta)
        case removed(id: String)
        case updated(meta: RecordingMeta)
    }

    /// 当前快照（任意时刻可读）
    public private(set) var recordings: [RecordingMeta] = []
    /// 变更事件流
    public nonisolated let changes: AsyncStream<Change>

    private let store: FileLibraryStore
    private let logger = Logger(subsystem: "com.beeuc.beecord.buzzlink", category: "catalog")
    private let changeContinuation: AsyncStream<Change>.Continuation

    /// manifest 条目（meta.json 修改时间，秒；增量比对用）
    private struct ManifestEntry: Codable, Sendable {
        var metaModifiedAt: TimeInterval
    }
    private var manifest: [String: ManifestEntry] = [:]
    private var manifestURL: URL {
        store.libraryURL.appendingPathComponent(".catalog.json")
    }

    public init(store: FileLibraryStore) {
        self.store = store
        let stream = AsyncStream<Change>.makeStream()
        self.changes = stream.stream
        self.changeContinuation = stream.continuation
    }

    /// 全量扫描建目录（app 启动调用）；随后增量刷新走 ``refresh``。
    public func scanAll() async {
        do {
            let metas = try store.listRecordings()
            var seen = Set<String>()
            var newManifest: [String: ManifestEntry] = [:]
            for meta in metas {
                seen.insert(meta.id)
                newManifest[meta.id] = ManifestEntry(metaModifiedAt: (try? modifiedAt(of: meta.id)) ?? 0)
                if let old = manifest[meta.id] {
                    if old.metaModifiedAt != (newManifest[meta.id]?.metaModifiedAt ?? 0) {
                        emit(.updated(meta: meta))
                    }
                } else {
                    emit(.added(meta: meta))
                }
            }
            for oldID in manifest.keys where !seen.contains(oldID) {
                emit(.removed(id: oldID))
            }
            manifest = newManifest
            recordings = metas
            persistManifest()
        } catch {
            logger.error("录音库全量扫描失败：\(error.localizedDescription)")
        }
    }

    /// 增量刷新：仅重读 manifest 中 mtime 变化的目录（与扫描间隙内的外部改动对账）。
    public func refresh() async {
        let known = Set(recordings.map(\.id))
        guard let metas = try? store.listRecordings() else { return }
        var current = Set<String>()
        for meta in metas {
            current.insert(meta.id)
            let mtime = (try? modifiedAt(of: meta.id)) ?? 0
            let old = manifest[meta.id]?.metaModifiedAt
            if old == nil {
                emit(.added(meta: meta))
                recordings.removeAll { $0.id == meta.id }
                recordings.append(meta)
            } else if old != mtime {
                emit(.updated(meta: meta))
                if let index = recordings.firstIndex(where: { $0.id == meta.id }) {
                    recordings[index] = meta
                }
            }
            manifest[meta.id] = ManifestEntry(metaModifiedAt: mtime)
        }
        for removedID in known.subtracting(current) {
            emit(.removed(id: removedID))
            recordings.removeAll { $0.id == removedID }
            manifest.removeValue(forKey: removedID)
        }
        persistManifest()
    }

    /// 本地操作后的即时更新（删除/重命名/转码回填，不再重扫磁盘）。
    public func applyLocalRemoval(id: String) {
        recordings.removeAll { $0.id == id }
        manifest.removeValue(forKey: id)
        emit(.removed(id: id))
        persistManifest()
    }

    /// 本地更新（meta 内容已变、文件已写）：直接替换快照并广播。
    public func applyLocalUpdate(_ meta: RecordingMeta) {
        if let index = recordings.firstIndex(where: { $0.id == meta.id }) {
            recordings[index] = meta
        } else {
            recordings.append(meta)
        }
        manifest[meta.id] = ManifestEntry(metaModifiedAt: (try? modifiedAt(of: meta.id)) ?? 0)
        emit(.updated(meta: meta))
        persistManifest()
    }

    // MARK: - 私有

    private func modifiedAt(of id: String) throws -> TimeInterval {
        let url = store.libraryURL.appendingPathComponent(id).appendingPathComponent("meta.json")
        let values = try url.resourceValues(forKeys: [.contentModificationDateKey])
        return values.contentModificationDate?.timeIntervalSince1970 ?? 0
    }

    private func persistManifest() {
        do {
            try FileManager.default.createDirectory(at: store.libraryURL, withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.sortedKeys]
            let data = try encoder.encode(manifest)
            try data.write(to: manifestURL, options: .atomic)
        } catch {
            logger.error("catalog manifest 写入失败：\(error.localizedDescription)")
        }
    }

    private func emit(_ change: Change) {
        changeContinuation.yield(change)
    }
}
