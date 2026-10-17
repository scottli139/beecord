import AVFoundation
import Foundation

/// 播放器引擎（studio-tech-design §5.1 决策 4：AVAudioEngine + AVAudioPlayerNode +
/// AVAudioUnitTimePitch，1/1.5/2 倍速）。
///
/// 线程模型：actor。状态机 `idle → loading → paused ⇄ playing →（播完/停止）idle`；
/// seek/skip/rate 在 paused/playing 均可用。核心逻辑与音频 IO 经 ``AudioPlaybackBackend``
/// 解耦——单测注入 Mock 后端与时钟（不进音频设备），真实后端为
/// ``AVAudioEngineBackend``（AVAudioPlayerNode 无内建进度，位置按
/// `lastRenderTime` 换算，进度节流发快照）。
public actor PlayerEngine {

    /// 播放状态
    public enum State: Sendable, Equatable {
        case idle
        case loading
        case paused
        case playing
    }

    /// 播放进度快照
    public struct Progress: Sendable, Equatable {
        public var position: TimeInterval
        public var duration: TimeInterval
        public var fraction: Double
        public var isPlaying: Bool

        public init(position: TimeInterval, duration: TimeInterval, fraction: Double, isPlaying: Bool) {
            self.position = position
            self.duration = duration
            self.fraction = fraction
            self.isPlaying = isPlaying
        }
    }

    /// 播放错误（具名）
    public enum PlaybackError: Error, LocalizedError, Sendable, Equatable {
        case notLoaded
        case loadFailed(String)
        case commandFailed(String)

        public var errorDescription: String? {
            switch self {
            case .notLoaded: "尚未载入音频"
            case .loadFailed(let message): "载入失败：\(message)"
            case .commandFailed(let message): "播放命令失败：\(message)"
            }
        }
    }

    /// 进度节流流（去重后产出；初始不回放）
    public nonisolated let progress: AsyncStream<Progress>

    public private(set) var state: State = .idle
    public private(set) var duration: TimeInterval = 0
    public private(set) var rate: Float = 1

    private let backend: any AudioPlaybackBackend
    private let progressInterval: TimeInterval
    private var progressContinuation: AsyncStream<Progress>.Continuation
    private var pumpTask: Task<Void, Never>?
    private var lastEmitted: Progress?

    public init(backend: any AudioPlaybackBackend, progressInterval: TimeInterval = 0.1) {
        self.backend = backend
        self.progressInterval = progressInterval
        let stream = AsyncStream<Progress>.makeStream()
        self.progress = stream.stream
        self.progressContinuation = stream.continuation
    }

    deinit {
        progressContinuation.finish()
    }

    // MARK: - 命令

    /// 载入音频（任意态可重入：先停止后端）。
    public func load(url: URL) async {
        await backend.stop()
        stopPump()
        state = .loading
        lastEmitted = nil
        do {
            duration = try await backend.load(url: url)
            state = .paused
            emit(Progress(position: 0, duration: duration, fraction: duration > 0 ? 0 : 1, isPlaying: false))
        } catch {
            duration = 0
            state = .idle
            emit(Progress(position: 0, duration: 0, fraction: 0, isPlaying: false))
        }
    }

    /// 播放（paused → playing；loading/idle 忽略）。
    public func play() async {
        guard state == .paused else { return }
        await backend.play()
        state = .playing
        startPump()
    }

    /// 暂停（playing → paused）。
    public func pause() async {
        guard state == .playing else { return }
        await backend.pause()
        state = .paused
        stopPump()
        emit(currentProgress(isPlaying: false))
    }

    /// 播放/暂停切换。
    public func togglePlay() async {
        if state == .playing { await pause() } else if state == .paused { await play() }
    }

    /// 定位（paused/playing 可用；越界钳制到 [0, duration]）。
    public func seek(to position: TimeInterval) async {
        guard state == .paused || state == .playing else { return }
        let target = min(max(0, position), duration)
        await backend.seek(to: target)
        lastEmitted = Progress(
            position: target,
            duration: duration,
            fraction: duration > 0 ? target / duration : 0,
            isPlaying: state == .playing
        )
        emit(currentProgress(isPlaying: state == .playing))
    }

    /// 相对跳转（±15s 走带）。
    public func skip(by delta: TimeInterval) async {
        guard state == .paused || state == .playing else { return }
        await seek(to: currentPositionEstimate() + delta)
    }

    /// 倍速（1/1.5/2；其余值忽略）。
    public func setRate(_ newRate: Float) async {
        guard newRate == 1 || newRate == 1.5 || newRate == 2 else { return }
        rate = newRate
        await backend.setRate(newRate)
    }

    /// 停止并回 idle（删除文件/切换录音时调用）。
    public func stop() async {
        await backend.stop()
        state = .idle
        duration = 0
        stopPump()
        emit(Progress(position: 0, duration: 0, fraction: 0, isPlaying: false))
    }

    // MARK: - 私有

    private func currentPositionEstimate() -> TimeInterval {
        lastEmitted?.position ?? 0
    }

    private func currentProgress(isPlaying: Bool) -> Progress {
        let position = min(currentPositionEstimate(), duration)
        return Progress(
            position: position,
            duration: duration,
            fraction: duration > 0 ? position / duration : 0,
            isPlaying: isPlaying
        )
    }

    private func emit(_ value: Progress) {
        guard value != lastEmitted else { return }
        lastEmitted = value
        progressContinuation.yield(value)
    }

    private func startPump() {
        guard pumpTask == nil else { return }
        let interval = progressInterval
        let playback = self
        pumpTask = Task { [weak playback] in
            while !Task.isCancelled {
                guard let playback else { return }
                await playback.progressTick()
                try? await Task.sleep(for: .milliseconds(max(10, Int(interval * 1000))))
            }
        }
    }

    private func stopPump() {
        pumpTask?.cancel()
        pumpTask = nil
    }

    private func progressTick() async {
        guard state == .playing else { return }
        let position = await backend.currentPosition()
        let clamped = min(max(0, position), duration)
        if duration > 0, clamped >= duration - 0.05 {
            // 播完：收敛 idle
            await backend.stop()
            state = .idle
            stopPump()
            emit(Progress(position: duration, duration: duration, fraction: 1, isPlaying: false))
            emit(Progress(position: 0, duration: duration, fraction: 0, isPlaying: false))
            return
        }
        lastEmitted = Progress(position: clamped, duration: duration, fraction: duration > 0 ? clamped / duration : 0, isPlaying: true)
        emit(currentProgress(isPlaying: true))
    }
}

/// 音频 IO 后端抽象（PlayerEngine 逻辑单测注入 Mock；真实实现 ``AVAudioEngineBackend``）。
public protocol AudioPlaybackBackend: Sendable {
    /// 载入音频；返回总时长（秒）
    func load(url: URL) async throws -> TimeInterval
    func play() async
    func pause() async
    /// 定位（秒）
    func seek(to position: TimeInterval) async
    func setRate(_ rate: Float) async
    func stop() async
    /// 当前播放位置（秒）
    func currentPosition() async -> TimeInterval
}

/// AVAudioEngine 真实后端：AVAudioPlayerNode + AVAudioUnitTimePitch。
public final class AVAudioEngineBackend: AudioPlaybackBackend, @unchecked Sendable {
    private let engine = AVAudioEngine()
    private let playerNode = AVAudioPlayerNode()
    private let timePitch = AVAudioUnitTimePitch()
    private var audioFile: AVAudioFile?
    private var positionOffset: TimeInterval = 0
    private var hasScheduled = false

    public init() {
        engine.attach(playerNode)
        engine.attach(timePitch)
        engine.connect(playerNode, to: timePitch, format: nil)
        engine.connect(timePitch, to: engine.mainMixerNode, format: nil)
    }

    public func load(url: URL) async throws -> TimeInterval {
        await stop()
        let file = try AVAudioFile(forReading: url)
        audioFile = file
        positionOffset = 0
        hasScheduled = false
        let duration = Double(file.length) / file.processingFormat.sampleRate
        return duration
    }

    public func play() async {
        guard audioFile != nil, !playerNode.isPlaying else { return }
        if !engine.isRunning {
            try? engine.start()
        }
        if !hasScheduled {
            scheduleFromCurrentPosition()
            hasScheduled = true
        }
        playerNode.play()
    }

    public func pause() async {
        positionOffset = currentPosition()
        playerNode.pause()
    }

    public func seek(to position: TimeInterval) async {
        guard audioFile != nil else { return }
        positionOffset = min(max(0, position), currentDuration())
        playerNode.stop()
        scheduleFromCurrentPosition()
        hasScheduled = true
        if playerNode.isPlaying {
            playerNode.play()
        }
    }

    public func setRate(_ rate: Float) async {
        timePitch.rate = rate
    }

    public func stop() async {
        playerNode.stop()
        engine.stop()
        audioFile = nil
        positionOffset = 0
    }

    public func currentPosition() -> TimeInterval {
        guard let nodeTime = playerNode.lastRenderTime,
              let playerTime = playerNode.playerTime(forNodeTime: nodeTime),
              let audioFile else {
            return positionOffset
        }
        let sampleRate = audioFile.processingFormat.sampleRate
        return positionOffset + Double(playerTime.sampleTime) / sampleRate
    }

    // MARK: - 私有

    private func currentDuration() -> TimeInterval {
        guard let audioFile else { return 0 }
        return Double(audioFile.length) / audioFile.processingFormat.sampleRate
    }

    private func scheduleFromCurrentPosition() {
        guard let audioFile else { return }
        let sampleRate = audioFile.processingFormat.sampleRate
        let startFrame = AVAudioFramePosition(positionOffset * sampleRate)
        let totalFrames = audioFile.length
        guard startFrame < totalFrames else { return }
        let frameCount = AVAudioFrameCount(totalFrames - startFrame)
        playerNode.scheduleSegment(audioFile, startingFrame: startFrame, frameCount: frameCount, at: nil)
    }
}
