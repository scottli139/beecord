import AVFoundation
import BuzzLinkCore
import Foundation

/// ogg → m4a 转码器（studio-tech-design §5.1 决策 3 的落地）：
/// 自研 Ogg demuxer（BuzzLinkCore）→ CoreAudio AudioConverter 分批解码 Opus →
/// AVAssetWriter 写 AAC（默认 48 kbps，保持源采样率与声道）。
///
/// 线程模型：actor。合作式取消（批间检查，抛 `CancellationError`，调用侧映射
/// ``Outcome/cancelled``，同 T5 取消语义）；产物先写 `audio.m4a.tmp` 再同卷原子 move
/// （崩溃不留半文件，`.tmp` 由上层启动清理）；幂等——目标已存在且非空时直接跳过。
public actor Transcoder {

    /// 转码错误（具名；UI 映射"转码失败"行态）
    public enum TranscodeError: Error, LocalizedError, Sendable, Equatable {
        case notOpusFile
        case emptyInput
        case writerFailed(String)
        case decoderFailed(String)

        public var errorDescription: String? {
            switch self {
            case .notOpusFile: "不是 Opus 封装（OpusHead 缺失或参数非法）"
            case .emptyInput: "Ogg 文件不含音频分包"
            case .writerFailed(let message): "m4a 写入失败：\(message)"
            case .decoderFailed(let message): "Opus 解码失败：\(message)"
            }
        }
    }

    /// 转码结果
    public enum Outcome: Sendable, Equatable {
        case transcribed      // 本次完成
        case alreadyExisted   // 目标已存在且非空（幂等跳过）
        case cancelled
    }

    /// 进度回调（0–1，每解码批一次）
    public typealias ProgressHandler = @Sendable (Double) -> Void

    private let decodeBatchPackets = 50

    public init() {}

    /// 执行转码（源文件与目标父目录须已存在）。
    public func transcribe(
        oggURL: URL,
        m4aURL: URL,
        bitRate: Int = 48_000,
        progress: ProgressHandler? = nil
    ) async -> Result<Outcome, TranscodeError> {
        do {
            return .success(try await perform(oggURL: oggURL, m4aURL: m4aURL, bitRate: bitRate, progress: progress))
        } catch is CancellationError {
            return .success(.cancelled)
        } catch let error as TranscodeError {
            return .failure(error)
        } catch let error as OggDemuxer.OggDemuxError {
            return .failure(.decoderFailed(error.localizedDescription))
        } catch {
            return .failure(.writerFailed(error.localizedDescription))
        }
    }

    // MARK: - 私有：主流程

    private func perform(
        oggURL: URL,
        m4aURL: URL,
        bitRate: Int,
        progress: ProgressHandler?
    ) async throws -> Outcome {
        if let size = try? FileManager.default.attributesOfItem(atPath: m4aURL.path)[.size] as? Int,
           size > 0 {
            return .alreadyExisted
        }
        guard let data = try? Data(contentsOf: oggURL) else {
            throw TranscodeError.writerFailed("源文件不可读：\(oggURL.lastPathComponent)")
        }
        let packets = try OggDemuxer.extractOpusPackets(data: data)
        guard let head = packets.first,
              head.count >= 19,
              head.starts(with: [0x4F, 0x70, 0x75, 0x73]) /* "Opus" */ else {
            throw TranscodeError.notOpusFile
        }
        let channels = Int(head[9])
        let preSkip = Int(readUInt16(head, 10))
        let sourceRate = Int(readUInt32(head, 12))
        guard channels >= 1, sourceRate > 0 else { throw TranscodeError.notOpusFile }
        // 音频分包 = 去 ID 头与 OpusTags 评论头（二者均非音频帧）
        let audioPackets = packets.dropFirst().filter { !$0.starts(with: [0x4F, 0x70, 0x75, 0x73, 0x54, 0x61, 0x67, 0x73]) /* "OpusTags" */ }
        guard !audioPackets.isEmpty else { throw TranscodeError.emptyInput }

        let chunks = try decodeBatches(
            packets: audioPackets,
            sourceRate: sourceRate,
            channels: channels,
            preSkip: preSkip,
            progress: progress
        )

        // 临时名保留 .m4a 扩展名（AVAssetWriter 按扩展名校验 fileType）；
        // `tmp-` 前缀供启动清理识别，完成后同卷原子 move
        let tmpURL = m4aURL.deletingLastPathComponent()
            .appendingPathComponent("tmp-" + m4aURL.lastPathComponent)
        try? FileManager.default.removeItem(at: tmpURL)
        do {
            try await writeM4A(chunks: chunks, tmpURL: tmpURL, sourceRate: sourceRate, channels: channels, bitRate: bitRate)
            try FileManager.default.moveItem(at: tmpURL, to: m4aURL)
        } catch {
            try? FileManager.default.removeItem(at: tmpURL)
            throw error
        }
        return .transcribed
    }

    // MARK: - 私有：解码（AudioConverter，分批）

    /// AVAudioConverter Opus 解码（系统解码器走 AVAudioConverter 驱动；直接
    /// AudioConverter 对 Opus 为空操作——2026-10-17 实测）。输出 48 kHz Float32。
    private func decodeBatches(
        packets: [Data],
        sourceRate: Int,
        channels: Int,
        preSkip: Int,
        progress: ProgressHandler?
    ) throws -> [Data] {
        var sourceDesc = AudioStreamBasicDescription(
            mSampleRate: 48000, // 系统 Opus 解码器原生速率
            mFormatID: kAudioFormatOpus,
            mFormatFlags: 0,
            mBytesPerPacket: 0,
            mFramesPerPacket: 960,
            mBytesPerFrame: 0,
            mChannelsPerFrame: UInt32(channels),
            mBitsPerChannel: 0,
            mReserved: 0
        )
        var pcmDesc = AudioStreamBasicDescription(
            mSampleRate: 48000,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
            mBytesPerPacket: 4 * UInt32(channels),
            mFramesPerPacket: 1,
            mBytesPerFrame: 4 * UInt32(channels),
            mChannelsPerFrame: UInt32(channels),
            mBitsPerChannel: 32,
            mReserved: 0
        )
        guard let sourceFormat = AVAudioFormat(streamDescription: &sourceDesc),
              let pcmFormat = AVAudioFormat(streamDescription: &pcmDesc),
              let converter = AVAudioConverter(from: sourceFormat, to: pcmFormat) else {
            throw TranscodeError.decoderFailed("AVAudioConverter 创建失败")
        }

        final class Feeder: @unchecked Sendable {
            let packets: [Data]
            var nextIndex = 0
            init(packets: [Data]) { self.packets = packets }
        }
        let feeder = Feeder(packets: packets)
        var chunks: [Data] = []
        var preSkipRemaining = preSkip // 域即 48 kHz 样本数（Opus 规范）
        var outputDone = false

        while !outputDone {
            if Task.isCancelled { throw CancellationError() }
            guard let outputBuffer = AVAudioPCMBuffer(pcmFormat: pcmFormat, frameCapacity: 4800) else {
                throw TranscodeError.decoderFailed("PCM 输出缓冲分配失败")
            }
            var convertError: NSError?
            let status = converter.convert(to: outputBuffer, error: &convertError) { _, inputStatus in
                guard feeder.nextIndex < feeder.packets.count else {
                    inputStatus.pointee = .noDataNow
                    return nil
                }
                let packet = feeder.packets[feeder.nextIndex]
                feeder.nextIndex += 1
                let inputBuffer = AVAudioCompressedBuffer(
                    format: sourceFormat,
                    packetCapacity: 1,
                    maximumPacketSize: packet.count
                )
                inputBuffer.packetCount = 1
                inputBuffer.byteLength = UInt32(packet.count)
                packet.withUnsafeBytes { rawBuffer in
                    if let base = rawBuffer.baseAddress {
                        inputBuffer.data.copyMemory(from: base, byteCount: packet.count)
                    }
                }
                inputBuffer.packetDescriptions?[0] = AudioStreamPacketDescription(
                    mStartOffset: 0,
                    mVariableFramesInPacket: 960,
                    mDataByteSize: UInt32(packet.count)
                )
                inputStatus.pointee = .haveData
                return inputBuffer
            }
            if status == .error {
                throw TranscodeError.decoderFailed(convertError?.localizedDescription ?? "未知解码错误")
            }
            if let channelData = outputBuffer.floatChannelData, outputBuffer.frameLength > 0 {
                let frameLength = Int(outputBuffer.frameLength)
                let values = (0..<(frameLength * channels)).map { channelData[0][$0] }
                if preSkipRemaining > 0 {
                    let drop = min(preSkipRemaining, frameLength)
                    chunks.append(floatBytes(Array(values.dropFirst(drop * channels))))
                    preSkipRemaining = 0
                } else {
                    chunks.append(floatBytes(values))
                }
            }
            progress?(Double(feeder.nextIndex) / Double(max(1, packets.count)))
            if feeder.nextIndex >= packets.count, outputBuffer.frameLength == 0 { outputDone = true }
            if status == .endOfStream { outputDone = true }
        }
        return chunks
    }

    /// Float32 交织样本 → 原始字节
    private func floatBytes(_ samples: [Float]) -> Data {
        samples.withUnsafeBytes { Data($0) }
    }

    // MARK: - 私有：写 m4a

    private func writeM4A(
        chunks: [Data],
        tmpURL: URL,
        sourceRate: Int,
        channels: Int,
        bitRate: Int
    ) async throws {
        // 产物统一 48 kHz（系统 Opus 解码器原生输出速率；AAC 重采样交给编码器）
        let outputRate = 48_000
        let writer = try AVAssetWriter(url: tmpURL, fileType: .m4a)
        let outputSettings: [String: Any] = [
            AVFormatIDKey: kAudioFormatMPEG4AAC,
            AVSampleRateKey: outputRate,
            AVNumberOfChannelsKey: channels,
            AVEncoderBitRateKey: bitRate
        ]
        let input = AVAssetWriterInput(mediaType: .audio, outputSettings: outputSettings)
        input.expectsMediaDataInRealTime = false
        guard writer.canAdd(input) else { throw TranscodeError.writerFailed("AVAssetWriter 拒绝音频输入") }
        writer.add(input)
        guard writer.startWriting() else {
            throw TranscodeError.writerFailed("startWriting 失败：\(writer.error?.localizedDescription ?? "未知")")
        }
        writer.startSession(atSourceTime: .zero)

        do {
            try await appendAllChunks(chunks: chunks, input: input, outputRate: outputRate, channels: channels)
        } catch is CancellationError {
            writer.cancelWriting()
            throw CancellationError()
        }
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            writer.finishWriting { continuation.resume() }
        }
        guard writer.status == .completed else {
            throw TranscodeError.writerFailed("finishWriting 未完成：\(writer.error?.localizedDescription ?? "未知")")
        }
    }

    /// 将全部 PCM 块写入；取消时抛 `CancellationError`。
    private func appendAllChunks(
        chunks: [Data],
        input: AVAssetWriterInput,
        outputRate: Int,
        channels: Int
    ) async throws {
        let bytesPerFrame = 4 * channels
        var formatDesc: CMFormatDescription?
        var pcmDesc = AudioStreamBasicDescription(
            mSampleRate: Double(outputRate),
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
            mBytesPerPacket: UInt32(bytesPerFrame),
            mFramesPerPacket: 1,
            mBytesPerFrame: UInt32(bytesPerFrame),
            mChannelsPerFrame: UInt32(channels),
            mBitsPerChannel: 32,
            mReserved: 0
        )
        let descStatus = CMAudioFormatDescriptionCreate(
            allocator: kCFAllocatorDefault,
            asbd: &pcmDesc,
            layoutSize: 0,
            layout: nil,
            magicCookieSize: 0,
            magicCookie: nil,
            extensions: nil,
            formatDescriptionOut: &formatDesc
        )
        guard descStatus == noErr, let formatDesc else {
            throw TranscodeError.writerFailed("PCM 格式描述创建失败：\(descStatus)")
        }

        var nextChunk = 0
        var presentation = CMTime.zero

        while nextChunk < chunks.count {
            if Task.isCancelled {
                throw CancellationError()
            }
            guard input.isReadyForMoreMediaData else {
                try await Task.sleep(for: .milliseconds(1))
                continue
            }
            let chunk = chunks[nextChunk]
            let frameCount = chunk.count / bytesPerFrame
            let sampleBuffer = try makeSampleBuffer(
                pcm: chunk,
                frameCount: frameCount,
                channels: channels,
                sampleRate: outputRate,
                formatDescription: formatDesc,
                presentation: presentation
            )
            guard input.append(sampleBuffer) else {
                throw TranscodeError.writerFailed("append 失败")
            }
            presentation = presentation + CMTime(value: Int64(frameCount), timescale: CMTimeScale(outputRate))
            nextChunk += 1
        }
        input.markAsFinished()
    }

    private func makeSampleBuffer(
        pcm: Data,
        frameCount: Int,
        channels: Int,
        sampleRate: Int,
        formatDescription: CMFormatDescription,
        presentation: CMTime
    ) throws -> CMSampleBuffer {
        let bytes = pcm.count
        var blockBuffer: CMBlockBuffer?
        let blockStatus = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault,
            memoryBlock: nil,
            blockLength: bytes,
            blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil,
            offsetToData: 0,
            dataLength: bytes,
            flags: 0,
            blockBufferOut: &blockBuffer
        )
        guard blockStatus == noErr, let blockBuffer else {
            throw TranscodeError.writerFailed("BlockBuffer 创建失败：\(blockStatus)")
        }
        try pcm.withUnsafeBytes { rawBuffer in
            guard let base = rawBuffer.baseAddress else {
                throw TranscodeError.writerFailed("PCM 数据为空")
            }
            let replaceStatus = CMBlockBufferReplaceDataBytes(
                with: base,
                blockBuffer: blockBuffer,
                offsetIntoDestination: 0,
                dataLength: bytes
            )
            guard replaceStatus == noErr else {
                throw TranscodeError.writerFailed("BlockBuffer 填充失败：\(replaceStatus)")
            }
        }
        var timing = CMSampleTimingInfo(
            duration: CMTime(value: 1, timescale: CMTimeScale(sampleRate)),
            presentationTimeStamp: presentation,
            decodeTimeStamp: .invalid
        )
        var sampleBuffer: CMSampleBuffer?
        let sampleStatus = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: blockBuffer,
            formatDescription: formatDescription,
            sampleCount: frameCount,
            sampleTimingEntryCount: 1,
            sampleTimingArray: &timing,
            sampleSizeEntryCount: 0,
            sampleSizeArray: nil,
            sampleBufferOut: &sampleBuffer
        )
        guard sampleStatus == noErr, let sampleBuffer else {
            throw TranscodeError.writerFailed("SampleBuffer 创建失败：\(sampleStatus)")
        }
        return sampleBuffer
    }

    // MARK: - 私有：杂项

    private func readUInt16(_ data: Data, _ offset: Int) -> UInt16 {
        UInt16(data[offset]) | (UInt16(data[offset + 1]) << 8)
    }

    private func readUInt32(_ data: Data, _ offset: Int) -> UInt32 {
        UInt32(data[offset]) | (UInt32(data[offset + 1]) << 8) | (UInt32(data[offset + 2]) << 16) | (UInt32(data[offset + 3]) << 24)
    }
}

/// AudioConverter 输入盒：包数组 + 游标 + 当前包描述。
/// 回调为 @convention(c) 无捕获；包指针与描述在回调闭包内设置（转换器同步消费）。
private final class ConverterInput {
    let packets: [Data]
    var nextIndex = 0
    var currentPacket: Data?
    var exhausted = false
    var descriptions = [AudioStreamPacketDescription](
        repeating: AudioStreamPacketDescription(),
        count: 1
    )

    init(packets: [Data]) {
        self.packets = packets
    }
}

private func converterInputProc(
    _ inAudioConverter: AudioConverterRef,
    _ ioNumberDataPackets: UnsafeMutablePointer<UInt32>,
    _ ioData: UnsafeMutablePointer<AudioBufferList>,
    _ outPacketDescriptions: UnsafeMutablePointer<UnsafeMutablePointer<AudioStreamPacketDescription>?>?,
    _ inClientData: UnsafeMutableRawPointer?
) -> OSStatus {
    guard let inClientData else { return -1 }
    let input = Unmanaged<ConverterInput>.fromOpaque(inClientData).takeUnretainedValue()
    guard input.nextIndex < input.packets.count else {
        // EOF 惯例：noErr + 0 包（哨兵错误会导致转换器重试死循环）
        ioNumberDataPackets.pointee = 0
        input.exhausted = true
        return noErr
    }
    let packet = input.packets[input.nextIndex]
    input.currentPacket = packet
    input.descriptions[0] = AudioStreamPacketDescription(
        mStartOffset: 0,
        mVariableFramesInPacket: 0,
        mDataByteSize: UInt32(packet.count)
    )
    packet.withUnsafeBytes { rawBuffer in
        ioData.pointee.mBuffers.mData = UnsafeMutableRawPointer(mutating: rawBuffer.baseAddress)
        ioData.pointee.mBuffers.mDataByteSize = UInt32(packet.count)
        ioData.pointee.mBuffers.mNumberChannels = 1
    }
    input.descriptions.withUnsafeMutableBytes { rawBuffer in
        outPacketDescriptions?.pointee = rawBuffer.baseAddress?
            .assumingMemoryBound(to: AudioStreamPacketDescription.self)
    }
    ioNumberDataPackets.pointee = 1
    input.nextIndex += 1
    return noErr
}
