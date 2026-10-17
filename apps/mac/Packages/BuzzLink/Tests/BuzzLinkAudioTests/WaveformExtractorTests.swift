import BuzzLinkAudio
import XCTest

/// WaveformExtractor：转码产物 → 600 列 min/max、缓存命中一致性。
final class WaveformExtractorTests: XCTestCase {

    private var workDir: URL!

    override func setUpWithError() throws {
        workDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("waveform-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: workDir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: workDir)
    }

    func testPeaksFromTranscodedM4AWithCache() async throws {
        let bundle = try XCTUnwrap(Bundle.module.url(forResource: "sample-05s", withExtension: "ogg"))
        let ogg = workDir.appendingPathComponent("sample.ogg")
        try FileManager.default.copyItem(at: bundle, to: ogg)
        let m4a = workDir.appendingPathComponent("sample.m4a")
        let transcribeResult = await Transcoder().transcribe(oggURL: ogg, m4aURL: m4a)
        guard case .success(.transcribed) = transcribeResult else {
            return XCTFail("转码前置失败：\(transcribeResult)")
        }

        let peaks = try await WaveformExtractor.peaks(m4aURL: m4a)
        XCTAssertEqual(peaks.count, 600)
        XCTAssertTrue(peaks.allSatisfy { $0.min <= $0.max }, "每列 min ≤ max")
        XCTAssertTrue(peaks.contains { $0.max > 0.01 }, "正弦样本应有非零峰值")

        // 缓存命中：同参数二次提取返回同值（mtime 不变 → 不重算）
        let cached = try await WaveformExtractor.peaks(m4aURL: m4a)
        XCTAssertEqual(cached, peaks)
    }
}
