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

/// WaveformBinner：渲染层聚合（600 列 → 按宽度条数；min/max 语义保持）。
final class WaveformBinnerTests: XCTestCase {

    func testBarSpecCounts() {
        // 704pt ≈141 根、564pt ≈113 根（条宽 3 + 间隙 2）
        XCTAssertEqual(WaveformBinner.barSpec(width: 704).count, 141)
        XCTAssertEqual(WaveformBinner.barSpec(width: 564).count, 113)
        XCTAssertEqual(WaveformBinner.barSpec(width: 704).barWidth, 3)
        // 余量均分到间隙：总占满宽度
        let spec = WaveformBinner.barSpec(width: 700)
        let used = CGFloat(spec.count) * spec.barWidth + CGFloat(spec.count - 1) * spec.gap
        XCTAssertEqual(used, 700, accuracy: 0.5)
    }

    func testBarSpecExtremeNarrow() {
        let spec = WaveformBinner.barSpec(width: 120)
        XCTAssertGreaterThanOrEqual(spec.barWidth, 1)
        XCTAssertGreaterThanOrEqual(spec.count, 1)
        let used = CGFloat(spec.count) * spec.barWidth + CGFloat(max(0, spec.count - 1)) * spec.gap
        XCTAssertLessThanOrEqual(used, 120)
    }

    func testAggregateKeepsMinMaxSemantics() {
        let columns = (0..<600).map { index in
            WaveformExtractor.Column(
                min: Float(-(index % 10)) / 10,
                max: Float(index % 7) / 10
            )
        }
        let bars = WaveformBinner.aggregate(columns, targetCount: 141)
        XCTAssertEqual(bars.count, 141)
        // 聚合保持全局 min/max 语义：极值不丢、每组 min ≤ max
        let globalMin = columns.map(\.min).min()
        let globalMax = columns.map(\.max).max()
        XCTAssertEqual(bars.map(\.min).min(), globalMin)
        XCTAssertEqual(bars.map(\.max).max(), globalMax)
        for bar in bars {
            XCTAssertLessThanOrEqual(bar.min, bar.max)
        }
    }
}
