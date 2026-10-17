import AVFoundation
import Foundation

/// 音频导出（详情操作行；T6 Phase C）：m4a/ogg 直拷，wav 解码 m4a → LPCM 写 RIFF 头。
public enum AudioExporter {

    public enum ExportError: Error, LocalizedError, Sendable {
        case sourceUnreadable
        case decodeFailed(String)
        case writeFailed(String)

        public var errorDescription: String? {
            switch self {
            case .sourceUnreadable: "源文件不可读"
            case .decodeFailed(let message): "解码失败：\(message)"
            case .writeFailed(let message): "写入失败：\(message)"
            }
        }
    }

    /// 导出执行（覆盖 destination）。
    /// - Parameters:
    ///   - format: "m4a"（直拷）/ "ogg"（原件直拷）/ "wav"（解码重封装）
    public static func export(sourceDirectory: URL, format: String, destination: URL) async throws {
        switch format {
        case "m4a", "ogg":
            let source = sourceDirectory.appendingPathComponent("audio.\(format)")
            guard FileManager.default.fileExists(atPath: source.path) else { throw ExportError.sourceUnreadable }
            if FileManager.default.fileExists(atPath: destination.path) {
                try FileManager.default.removeItem(at: destination)
            }
            try FileManager.default.copyItem(at: source, to: destination)
        case "wav":
            let source = sourceDirectory.appendingPathComponent("audio.m4a")
            guard FileManager.default.fileExists(atPath: source.path) else { throw ExportError.sourceUnreadable }
            try await writeWAV(m4aURL: source, destination: destination)
        default:
            throw ExportError.writeFailed("不支持的格式：\(format)")
        }
    }

    // MARK: - 私有：m4a → WAV

    private static func writeWAV(m4aURL: URL, destination: URL) async throws {
        let asset = AVURLAsset(url: m4aURL)
        let tracks = try await asset.loadTracks(withMediaType: .audio)
        guard let track = tracks.first else { throw ExportError.sourceUnreadable }
        let sampleRate = try await track.load(.nominalFrameRate)
        let channels = try await track.load(.formatDescriptions).first
            .map { CMAudioFormatDescriptionGetStreamBasicDescription($0)?.pointee.mChannelsPerFrame ?? 1 } ?? 1
        let trackInfo = (track: track, sampleRate: Double(sampleRate), channels: channels)

        // 以 m4a 名义采样率输出 Int16（WAV 惯例；>48k 原样保留）
        let outRate = Int(trackInfo.sampleRate)
        let outChannels = Int(trackInfo.channels)
        let bytesPerFrame = 2 * outChannels

        let reader = try AVAssetReader(asset: asset)
        let output = AVAssetReaderTrackOutput(track: trackInfo.track, outputSettings: [
            AVFormatIDKey: kAudioFormatLinearPCM,
            AVSampleRateKey: outRate,
            AVNumberOfChannelsKey: outChannels,
            AVLinearPCMBitDepthKey: 16,
            AVLinearPCMIsFloatKey: false
        ])
        reader.add(output)
        guard reader.startReading() else {
            throw ExportError.decodeFailed(reader.error?.localizedDescription ?? "startReading 失败")
        }

        var pcm = Data()
        while let sampleBuffer = autoreleasepool(invoking: { output.copyNextSampleBuffer() }) {
            guard let blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { continue }
            let length = CMBlockBufferGetDataLength(blockBuffer)
            var chunk = Data(count: length)
            chunk.withUnsafeMutableBytes { raw in
                if let base = raw.baseAddress {
                    CMBlockBufferCopyDataBytes(blockBuffer, atOffset: 0, dataLength: length, destination: base)
                }
            }
            pcm.append(chunk)
        }
        if reader.status == .failed {
            throw ExportError.decodeFailed(reader.error?.localizedDescription ?? "读取中断")
        }

        // RIFF/WAVE 头
        var header = Data()
        func appendLE32(_ value: UInt32) {
            header.append(UInt8(value & 0xFF)); header.append(UInt8((value >> 8) & 0xFF))
            header.append(UInt8((value >> 16) & 0xFF)); header.append(UInt8((value >> 24) & 0xFF))
        }
        func appendLE16(_ value: UInt16) {
            header.append(UInt8(value & 0xFF)); header.append(UInt8((value >> 8) & 0xFF))
        }
        let byteRate = UInt32(outRate * bytesPerFrame)
        let blockAlign = UInt16(bytesPerFrame)
        let dataSize = UInt32(pcm.count)
        header.append(contentsOf: "RIFF".utf8)
        appendLE32(36 + dataSize)
        header.append(contentsOf: "WAVE".utf8)
        header.append(contentsOf: "fmt ".utf8)
        appendLE32(16)              // fmt 块长
        appendLE16(1)               // PCM
        appendLE16(UInt16(outChannels))
        appendLE32(UInt32(outRate))
        appendLE32(byteRate)
        appendLE16(blockAlign)
        appendLE16(16)              // 位深
        header.append(contentsOf: "data".utf8)
        appendLE32(dataSize)

        do {
            var fileData = header
            fileData.append(pcm)
            if FileManager.default.fileExists(atPath: destination.path) {
                try FileManager.default.removeItem(at: destination)
            }
            try fileData.write(to: destination, options: .atomic)
        } catch {
            throw ExportError.writeFailed(error.localizedDescription)
        }
    }
}
