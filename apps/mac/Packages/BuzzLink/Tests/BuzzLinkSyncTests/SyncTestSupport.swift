import BuzzLink
import BuzzLinkCore
import BuzzLinkSimCore
import BuzzLinkSync
import BuzzLinkTestSupport
import Foundation
import XCTest

/// SyncEngine 测试支撑：临时库目录、确定性样本、进程内 Sim 装配、快照/事件收集、有界轮询。
enum SyncTestSupport {

    /// 临时库根目录（每用例独立；调用方负责 defer 删除）
    static func makeTempRoot() -> URL {
        FileManager.default.temporaryDirectory
            .appendingPathComponent("buzzlinksync-\(UUID().uuidString)", isDirectory: true)
    }

    /// 确定性伪数据文件（协议层只关心长度与 CRC）
    static func makeFile(id: String, durationMs: UInt32 = 1_000, size: Int, seed: UInt64) -> SimFile {
        var rng = SplitMix64(seed: seed)
        return SimFile(
            id: id,
            durationMs: durationMs,
            data: Data((0..<size).map { _ in UInt8(truncatingIfNeeded: rng.next()) })
        )
    }

    /// 直接构造 meta（预置库内容用；id 由 commit 覆写）
    static func makeMeta(fileID: String, size: Int, crc16: UInt16, durationMs: UInt32) -> RecordingMeta {
        RecordingMeta(
            schemaVersion: RecordingMeta.currentSchemaVersion,
            id: "",
            title: fileID,
            source: RecordingMeta.Source(
                fileID: fileID, deviceName: "BCD-TEST", deviceModel: "beecord-sim", fwVersion: "0.1.0"
            ),
            durationMs: durationMs,
            sizeBytes: size,
            crc16: crc16,
            format: "ogg-opus",
            sampleRate: 16_000,
            channels: 1,
            recordedAt: RecordingMeta.parseRecordedAt(fromFileID: fileID),
            importedAt: Date(timeIntervalSince1970: 1_760_000_000),
            audio: RecordingMeta.AudioFiles(ogg: "audio.ogg", m4a: nil),
            transcribe: RecordingMeta.PipelineStatus(status: "none"),
            summary: RecordingMeta.PipelineStatus(status: "none")
        )
    }

    /// 解码库内 meta.json（与 FileLibraryStore 写入格式对齐）
    static func decodeMeta(at url: URL) throws -> RecordingMeta {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return try decoder.decode(RecordingMeta.self, from: Data(contentsOf: url))
    }

    /// 库内录音目录名列表（排除 .partial）
    static func libraryDirectoryNames(root: URL) throws -> [String] {
        let library = root.appendingPathComponent("Library", isDirectory: true)
        let names = try FileManager.default.contentsOfDirectory(atPath: library.path)
        return names.filter { $0 != ".partial" }.sorted()
    }

    /// .partial 目录内容（目录不存在视为空）
    static func partialFileNames(root: URL) -> [String] {
        let dir = root.appendingPathComponent("Library/.partial", isDirectory: true)
        let names = try? FileManager.default.contentsOfDirectory(atPath: dir.path)
        return names ?? []
    }
}

/// 测试用同步配置：缩短退避/节流/取消等待；keepalive 拉长避免干扰（专项用例再缩短）
extension SyncConfig {
    static var testDefault: SyncConfig {
        SyncConfig(
            autoSyncEnabled: true,
            reconnectDelays: [0.05, 0.1, 0.2, 0.3],
            keepaliveInterval: 60,
            progressThrottleInterval: 0.05,
            cancelConvergenceTimeout: 2
        )
    }
}

/// 一套同步夹具：进程内 SimEngine 对端 + 客户端 + 临时库 + 引擎
struct SyncFixture {
    let transport: SimEngineTransport
    let client: BuzzLinkClient
    let store: FileLibraryStore
    let engine: SyncEngine
    let root: URL
}

/// - Parameters:
///   - commandTimeout/chunkTimeout: 注入客户端超时。默认短值保持非节流用例的失败路径
///     快速收敛；节流用例在慢 runner 上块间隔会被调度拉长（8KB/s 名义 ~30ms/块），
///     看门狗必须显著大于该间隔，否则调度停顿误触发整文件重试（CI 抖动根因，见
///     SyncEngineRecoveryTests.testCancelSyncStopsQueue 修复记录）。
func makeSyncFixture(
    files: [SimFile],
    faults: SimFaults = SimFaults(),
    randomSeed: UInt64 = 0xBEEC_0DE1,
    config: SyncConfig = .testDefault,
    throttleBytesPerSecond: Int? = nil,
    commandTimeout: TimeInterval = 0.5,
    chunkTimeout: TimeInterval = 0.3
) -> SyncFixture {
    let simEngine = SimEngine(files: files, faults: faults, randomSeed: randomSeed)
    let transport = SimEngineTransport(engine: simEngine, throttleBytesPerSecond: throttleBytesPerSecond)
    let client = BuzzLinkClient(transport: transport, commandTimeout: commandTimeout, chunkTimeout: chunkTimeout)
    let root = SyncTestSupport.makeTempRoot()
    let store = FileLibraryStore(rootURL: root)
    let engine = SyncEngine(client: client, store: store, config: config)
    return SyncFixture(transport: transport, client: client, store: store, engine: engine, root: root)
}

/// 快照收集器（跨并发线程安全）
final class SnapshotCollector: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [SyncSnapshot] = []

    var values: [SyncSnapshot] { lock.withLock { storage } }
    var states: [SyncState] { lock.withLock { storage.map(\.state) } }

    func record(_ snapshot: SyncSnapshot) {
        lock.withLock { storage.append(snapshot) }
    }
}

/// 事件收集器（跨并发线程安全）
final class SyncEventCollector: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [SyncEvent] = []

    var values: [SyncEvent] { lock.withLock { storage } }

    func record(_ event: SyncEvent) {
        lock.withLock { storage.append(event) }
    }
}

/// 收集引擎快照流（返回的任务在测试结束时 cancel）
func collectSnapshots(of engine: SyncEngine, into collector: SnapshotCollector) -> Task<Void, Never> {
    let stream = engine.snapshots
    return Task {
        for await snapshot in stream {
            collector.record(snapshot)
        }
    }
}

/// 收集引擎事件流（返回的任务在测试结束时 cancel）
func collectEvents(of engine: SyncEngine, into collector: SyncEventCollector) -> Task<Void, Never> {
    let stream = engine.events
    return Task {
        for await event in stream {
            collector.record(event)
        }
    }
}

/// 有界轮询直到条件成立或超时（时序纪律：禁固定 sleep）
func pollUntil(timeout: TimeInterval, _ predicate: @escaping () async -> Bool) async -> Bool {
    let deadline = ProcessInfo.processInfo.systemUptime + timeout
    while ProcessInfo.processInfo.systemUptime < deadline {
        if await predicate() { return true }
        try? await Task.sleep(for: .milliseconds(10))
    }
    return await predicate()
}
