import AVFoundation
import BuzzLinkAudio
import XCTest

/// 转码链端到端：合成 ogg（tools/make_sim_samples.sh 产物，16kHz 单声道 Opus）
/// → m4a（AAC 48kbps）→ AVAssetReader 回读校验时长/能量。
final class TranscoderTests: XCTestCase {

    private var workDir: URL!

    override func setUpWithError() throws {
        workDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("transcoder-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: workDir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: workDir)
    }

    private func fixture(_ name: String) throws -> URL {
        let bundle = try XCTUnwrap(Bundle.module.url(forResource: name, withExtension: "ogg"))
        return bundle
    }

    private func tempCopy(_ name: String) throws -> URL {
        let source = try fixture(name)
        let destination = workDir.appendingPathComponent("\(name).ogg")
        try FileManager.default.copyItem(at: source, to: destination)
        return destination
    }

    /// AVAssetReader 回读：时长（秒）与 PCM 采样统计
    private func readBack(_ m4aURL: URL) async throws -> (duration: TimeInterval, sampleCount: Int, hasEnergy: Bool) {
        let asset = AVURLAsset(url: m4aURL)
        let duration = try await asset.load(.duration).seconds
        let tracks = try await asset.loadTracks(withMediaType: .audio)
        guard let track = tracks.first else { throw NSError(domain: "test", code: 1) }
        let reader = try AVAssetReader(asset: asset)
        let output = AVAssetReaderTrackOutput(track: track, outputSettings: [
            AVFormatIDKey: kAudioFormatLinearPCM,
            AVSampleRateKey: 16_000,
            AVNumberOfChannelsKey: 1,
            AVLinearPCMBitDepthKey: 16,
            AVLinearPCMIsFloatKey: false
        ])
        reader.add(output)
        guard reader.startReading() else { throw NSError(domain: "test", code: 2) }
        var sampleCount = 0
        var hasEnergy = false
        while let sampleBuffer = output.copyNextSampleBuffer() {
            guard let blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { continue }
            let length = CMBlockBufferGetDataLength(blockBuffer)
            var data = Data(count: length)
            data.withUnsafeMutableBytes { raw in
                _ = raw.baseAddress.map { CMBlockBufferCopyDataBytes(blockBuffer, atOffset: 0, dataLength: length, destination: $0) }
            }
            sampleCount += length / 2
            if data.contains(where: { $0 != 0 }) { hasEnergy = true }
        }
        return (duration, sampleCount, hasEnergy)
    }

    func testRoundTrip05s() async throws {
        let ogg = try tempCopy("sample-05s")
        let m4a = workDir.appendingPathComponent("sample-05s.m4a")
        let transcoder = Transcoder()
        let result = await transcoder.transcribe(oggURL: ogg, m4aURL: m4a)
        guard case .success(.transcribed) = result else {
            return XCTFail("期望 transcribed，实际 \(result)")
        }
        XCTAssertTrue(FileManager.default.fileExists(atPath: m4a.path))
        let info = try await readBack(m4a)
        XCTAssertGreaterThan(info.duration, 4.0, "时长应≈5s（预跳过裁剪后略短）")
        XCTAssertLessThan(info.duration, 5.8)
        XCTAssertGreaterThan(info.sampleCount, 16_000 * 3, "PCM 样本数应达数秒量级")
        XCTAssertTrue(info.hasEnergy, "正弦样本回读应非零能量")
    }

    func testCorruptOggFailsWithNamedError() async throws {
        let bad = workDir.appendingPathComponent("bad.ogg")
        try Data("this is not an ogg file".utf8).write(to: bad)
        let m4a = workDir.appendingPathComponent("bad.m4a")
        let transcoder = Transcoder()
        let result = await transcoder.transcribe(oggURL: bad, m4aURL: m4a)
        guard case .failure(let error) = result else {
            return XCTFail("期望失败，实际 \(result)")
        }
        if case .notOpusFile = error {
            // 非 OggS → demux 失败路径，合法
        } else {
            guard case .decoderFailed = error else {
                return XCTFail("期望 decoderFailed/notOpusFile，实际 \(error)")
            }
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: m4a.path))
    }

    func testCancelConvergesAndCleansUp() async throws {
        let ogg = try tempCopy("sample-30s")
        let m4a = workDir.appendingPathComponent("sample-30s.m4a")
        let transcoder = Transcoder()
        final class Box: @unchecked Sendable { var task: Task<Result<Transcoder.Outcome, Transcoder.TranscodeError>, Never>? }
        let box = Box()
        let task = Task {
            await transcoder.transcribe(oggURL: ogg, m4aURL: m4a) { _ in
                box.task?.cancel() // 首个进度回调即取消：批间检查确定收敛
            }
        }
        box.task = task
        let result = await task.value
        guard case .success(.cancelled) = result else {
            return XCTFail("期望 cancelled，实际 \(result)")
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: m4a.path), "取消后不应有产物")
        let tmpLeftover = workDir.appendingPathComponent("tmp-" + m4a.lastPathComponent)
        XCTAssertFalse(FileManager.default.fileExists(atPath: tmpLeftover.path), "取消后不应有临时文件残留")
    }

    func testSecondRunSkipsAsAlreadyExisted() async throws {
        let ogg = try tempCopy("sample-05s")
        let m4a = workDir.appendingPathComponent("sample-05s.m4a")
        let transcoder = Transcoder()
        let first = await transcoder.transcribe(oggURL: ogg, m4aURL: m4a)
        guard case .success(.transcribed) = first else { return XCTFail("首跑应成功：\(first)") }
        let second = await transcoder.transcribe(oggURL: ogg, m4aURL: m4a)
        guard case .success(.alreadyExisted) = second else {
            return XCTFail("期望 alreadyExisted，实际 \(second)")
        }
    }
}
