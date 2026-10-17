import BuzzLinkAudio
import XCTest

/// Mock 后端：记录命令序列 + 可控位置（PlayerEngine 逻辑不进音频设备）。
private final class MockBackend: AudioPlaybackBackend, @unchecked Sendable {
    var loadedURL: URL?
    var duration: TimeInterval = 30
    var position: TimeInterval = 0
    var calls: [String] = []
    var shouldFailLoad = false

    func load(url: URL) async throws -> TimeInterval {
        calls.append("load")
        if shouldFailLoad { throw NSError(domain: "mock", code: 9) }
        loadedURL = url
        return duration
    }
    func play() async { calls.append("play") }
    func pause() async { calls.append("pause") }
    func seek(to position: TimeInterval) async {
        calls.append("seek:\(Int(position))")
        self.position = position
    }
    func setRate(_ rate: Float) async { calls.append("rate:\(rate)") }
    func stop() async {
        calls.append("stop")
        position = 0
    }
    func currentPosition() async -> TimeInterval { position }
}

/// 有界等待辅助：轮询条件（禁固定 sleep）
private func waitUntil(
    _ condition: @escaping () async -> Bool,
    timeout: TimeInterval = 3,
    poll: TimeInterval = 0.02
) async -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if await condition() { return true }
        try? await Task.sleep(for: .milliseconds(Int(poll * 1000)))
    }
    return await condition()
}

final class PlayerEngineTests: XCTestCase {

    private func makeEngine(duration: TimeInterval = 30) -> (PlayerEngine, MockBackend) {
        let backend = MockBackend()
        backend.duration = duration
        let engine = PlayerEngine(backend: backend, progressInterval: 0.01)
        return (engine, backend)
    }

    func testLoadTransitionsToPausedAndEmitsInitialProgress() async throws {
        let (engine, backend) = makeEngine(duration: 34)
        var iterator = engine.progress.makeAsyncIterator()
        await engine.load(url: URL(fileURLWithPath: "/tmp/a.m4a"))
        let state = await engine.state
        XCTAssertEqual(state, .paused)
        let first = await iterator.next()
        XCTAssertEqual(first, PlayerEngine.Progress(position: 0, duration: 34, fraction: 0, isPlaying: false))
        XCTAssertEqual(backend.loadedURL?.lastPathComponent, "a.m4a")
    }

    func testLoadFailureReturnsToIdle() async {
        let (engine, backend) = makeEngine()
        backend.shouldFailLoad = true
        await engine.load(url: URL(fileURLWithPath: "/tmp/bad.m4a"))
        let state = await engine.state
        XCTAssertEqual(state, .idle)
    }

    func testPlayPauseToggleAndStateGuards() async {
        let (engine, backend) = makeEngine()
        await engine.load(url: URL(fileURLWithPath: "/tmp/a.m4a"))
        // idle/paused 态守卫：未 load 时 play 无效
        let fresh = PlayerEngine(backend: MockBackend())
        await fresh.play()
        let freshState = await fresh.state
        XCTAssertEqual(freshState, .idle)
        await fresh.pause()
        let freshStateAfterPause = await fresh.state
        XCTAssertEqual(freshStateAfterPause, .idle)

        await engine.play()
        let state1 = await engine.state
        XCTAssertEqual(state1, .playing)
        await engine.play() // playing 时重复 play 无效
        let state2 = await engine.state
        XCTAssertEqual(state2, .playing)
        await engine.pause()
        let state3 = await engine.state
        XCTAssertEqual(state3, .paused)
        XCTAssertEqual(backend.calls.filter { $0 == "play" }.count, 1)
        XCTAssertEqual(backend.calls.filter { $0 == "pause" }.count, 1)
    }

    func testTogglePlaySwitchesBothWays() async {
        let (engine, _) = makeEngine()
        await engine.load(url: URL(fileURLWithPath: "/tmp/a.m4a"))
        await engine.togglePlay()
        let state4 = await engine.state
        XCTAssertEqual(state4, .playing)
        await engine.togglePlay()
        let state5 = await engine.state
        XCTAssertEqual(state5, .paused)
    }

    func testSeekClampsAndSkipAppliesDelta() async {
        let (engine, backend) = makeEngine(duration: 60)
        await engine.load(url: URL(fileURLWithPath: "/tmp/a.m4a"))
        await engine.seek(to: 999)
        XCTAssertEqual(backend.position, 60, "seek 应钳制到时长上限")
        await engine.seek(to: -5)
        XCTAssertEqual(backend.position, 0, "seek 应钳制到 0")
        await engine.seek(to: 10)
        await engine.skip(by: 15)
        XCTAssertEqual(backend.position, 25)
        await engine.skip(by: -100)
        XCTAssertEqual(backend.position, 0)
    }

    func testSetRateOnlyAcceptsSupportedValues() async {
        let (engine, backend) = makeEngine()
        await engine.load(url: URL(fileURLWithPath: "/tmp/a.m4a"))
        await engine.setRate(1.5)
        let rate1 = await engine.rate
        XCTAssertEqual(rate1, 1.5)
        await engine.setRate(0.75)
        let rate2 = await engine.rate
        XCTAssertEqual(rate2, 1.5, "非法倍速应被忽略")
        await engine.setRate(2)
        let rate3 = await engine.rate
        XCTAssertEqual(rate3, 2)
        XCTAssertTrue(backend.calls.contains("rate:1.5"))
        XCTAssertFalse(backend.calls.contains("rate:0.75"))
    }

    func testNaturalFinishConvergesToIdle() async {
        let (engine, backend) = makeEngine(duration: 1)
        final class Flag: @unchecked Sendable { var value = false }
        let finished = Flag()
        let consume = Task {
            for await progress in engine.progress where progress.fraction == 1 && !progress.isPlaying {
                finished.value = true
                return
            }
        }
        await engine.load(url: URL(fileURLWithPath: "/tmp/a.m4a"))
        await engine.play()
        backend.position = 1 // 模拟播完位置
        let ok = await waitUntil { finished.value }
        consume.cancel()
        XCTAssertTrue(ok, "播完应发 fraction=1 快照")
        let state = await engine.state
        XCTAssertEqual(state, .idle)
    }

    func testStopResetsToIdle() async {
        let (engine, backend) = makeEngine()
        await engine.load(url: URL(fileURLWithPath: "/tmp/a.m4a"))
        await engine.play()
        await engine.stop()
        let state6 = await engine.state
        XCTAssertEqual(state6, .idle)
        XCTAssertEqual(backend.calls.last, "stop")
    }

    func testReloadDuringPlayingReplacesAndPauses() async {
        let (engine, backend) = makeEngine(duration: 10)
        await engine.load(url: URL(fileURLWithPath: "/tmp/old.m4a"))
        await engine.play()
        backend.duration = 42
        await engine.load(url: URL(fileURLWithPath: "/tmp/new.m4a"))
        let state7 = await engine.state
        XCTAssertEqual(state7, .paused)
        let duration1 = await engine.duration
        XCTAssertEqual(duration1, 42)
        XCTAssertEqual(backend.loadedURL?.lastPathComponent, "new.m4a")
        XCTAssertTrue(backend.calls.contains("stop"), "重载应先停止后端")
        XCTAssertEqual(backend.calls.filter { $0 == "load" }.count, 2, "应载入两次")
    }
}
