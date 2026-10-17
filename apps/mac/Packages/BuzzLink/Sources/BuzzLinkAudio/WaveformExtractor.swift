import AVFoundation
import Foundation

/// 波形峰值提取（T6 问题 3）：AVAssetReader 流式读 m4a → 分块 min/max（600 列），
/// 峰值缓存 `waveform.json`（记录源文件 size+mtime，变更即失效重建）。
/// 线程模型：后台队列同步读取（autoreleasepool 分块），async 包装； actor 隔离非必需。
public enum WaveformExtractor {

    /// 单列 min/max（-1...1，Float32 域）
    public struct Column: Codable, Sendable, Equatable {
        public var min: Float
        public var max: Float
    }

    /// 缓存文件结构（与录音同目录，meta 级联清理）
    private struct Cache: Codable, Sendable {
        var sourceSize: Int
        var sourceModifiedAt: TimeInterval
        var columns: [Column]
    }

    public enum ExtractionError: Error, LocalizedError, Sendable {
        case noAudioTrack
        case readerFailed(String)

        public var errorDescription: String? {
            switch self {
            case .noAudioTrack: "m4a 不含音频轨"
            case .readerFailed(let message): "波形读取失败：\(message)"
            }
        }
    }

    /// 提取（或命中缓存直接返回）`columnCount` 列 min/max。
    public static func peaks(m4aURL: URL, columnCount: Int = 600) async throws -> [Column] {
        let cacheURL = m4aURL.deletingLastPathComponent().appendingPathComponent("waveform.json")
        let attributes = try FileManager.default.attributesOfItem(atPath: m4aURL.path)
        let size = attributes[.size] as? Int ?? 0
        let modifiedAt = (attributes[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
        if let data = try? Data(contentsOf: cacheURL),
           let cache = try? JSONDecoder().decode(Cache.self, from: data),
           cache.sourceSize == size,
           cache.sourceModifiedAt == modifiedAt,
           cache.columns.count == columnCount {
            return cache.columns
        }
        let columns = try await extract(m4aURL: m4aURL, columnCount: columnCount)
        if let data = try? JSONEncoder().encode(Cache(sourceSize: size, sourceModifiedAt: modifiedAt, columns: columns)) {
            try? data.write(to: cacheURL, options: .atomic)
        }
        return columns
    }

    /// 使缓存失效（m4a 被替换时；当前实现以 size+mtime 判别，无需显式调用，保留 API 备用）。
    public static func invalidate(m4aURL: URL) {
        try? FileManager.default.removeItem(at: m4aURL.deletingLastPathComponent().appendingPathComponent("waveform.json"))
    }

    // MARK: - 私有

    /// 流式提取：轨道信息 await 加载；读取循环同步执行（autoreleasepool 分块，
    /// 峰值量级毫秒~秒级，不另起后台队列）。
    private static func extract(m4aURL: URL, columnCount: Int) async throws -> [Column] {
        let asset = AVURLAsset(url: m4aURL)
        let duration = try await asset.load(.duration).seconds
        let tracks = try await asset.loadTracks(withMediaType: .audio)
        guard let track = tracks.first else { throw ExtractionError.noAudioTrack }
        let sampleRate = 16_000
        let totalFrames = max(1, Int(duration * Double(sampleRate)))
        let framesPerColumn = max(1, totalFrames / columnCount)

        let reader = try AVAssetReader(asset: asset)
        let output = AVAssetReaderTrackOutput(track: track, outputSettings: [
            AVFormatIDKey: kAudioFormatLinearPCM,
            AVSampleRateKey: sampleRate,
            AVNumberOfChannelsKey: 1,
            AVLinearPCMBitDepthKey: 32,
            AVLinearPCMIsFloatKey: true
        ])
        reader.add(output)
        guard reader.startReading() else {
            throw ExtractionError.readerFailed(reader.error?.localizedDescription ?? "startReading 失败")
        }

        var columns: [Column] = []
        columns.reserveCapacity(columnCount)
        var bucketMin = Float.greatestFiniteMagnitude
        var bucketMax = -Float.greatestFiniteMagnitude
        var bucketFrames = 0
        var bufferedFrames = 0

        while let sampleBuffer = autoreleasepool(invoking: { output.copyNextSampleBuffer() }) {
            guard let blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { continue }
            let byteCount = CMBlockBufferGetDataLength(blockBuffer)
            let frameCount = byteCount / MemoryLayout<Float>.size
            guard frameCount > 0 else { continue }
            var data = [Float](repeating: 0, count: frameCount)
            data.withUnsafeMutableBytes { raw in
                if let base = raw.baseAddress {
                    CMBlockBufferCopyDataBytes(blockBuffer, atOffset: 0, dataLength: byteCount, destination: base)
                }
            }
            var index = 0
            while index < frameCount {
                let take = min(framesPerColumn - bucketFrames, frameCount - index)
                let slice = data[index..<(index + take)]
                for value in slice {
                    if value < bucketMin { bucketMin = value }
                    if value > bucketMax { bucketMax = value }
                }
                bucketFrames += take
                bufferedFrames += take
                index += take
                if bucketFrames == framesPerColumn {
                    columns.append(Column(min: bucketMin, max: bucketMax))
                    bucketMin = .greatestFiniteMagnitude
                    bucketMax = -.greatestFiniteMagnitude
                    bucketFrames = 0
                }
            }
        }
        if reader.status == .failed {
            throw ExtractionError.readerFailed(reader.error?.localizedDescription ?? "读取中断")
        }
        if bucketFrames > 0 {
            columns.append(Column(min: bucketMin, max: bucketMax))
        }
        // 短文件补足列数（保持 UI 恒定宽度语义）
        while columns.count < columnCount {
            columns.append(Column(min: 0, max: 0))
        }
        return Array(columns.prefix(columnCount))
    }
}
