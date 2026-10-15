import BuzzLinkCore
import Foundation
import Network
import os

/// Loopback TCP 传输（TESTING.md §4 同机开发/测试通道）：
/// 连接 `127.0.0.1:``BuzzLinkConstants/loopbackPort``，承载与 BLE 空口完全同构的
/// BuzzLink 字节流（§3.3 字节流重组天然适配流式 socket）。
///
/// 线程模型：全部可变状态限定于内部串行队列（NWConnection 的 start/receive 回调队列）；
/// 公共入口经该队列派发，故标记 `@unchecked Sendable`。入站/状态流跨连接复用
/// （断线重连后仍有效），直到本实例释放。
public final class LoopbackTransport: BuzzLinkTransport, @unchecked Sendable {

    public let stateStream: AsyncStream<BuzzLinkTransportState>
    public let inbound: AsyncStream<Data>

    private let host: NWEndpoint.Host
    private let port: NWEndpoint.Port
    private let queue = DispatchQueue(label: "com.beeuc.beecord.buzzlink.loopback")
    private let logger = Logger(subsystem: "com.beeuc.beecord.buzzlink", category: "transport")

    private let stateContinuation: AsyncStream<BuzzLinkTransportState>.Continuation
    private let inboundContinuation: AsyncStream<Data>.Continuation
    /// 以下成员仅在串行队列上访问
    private var connection: NWConnection?

    /// - Parameters:
    ///   - host: 默认 loopback（`127.0.0.1`）
    ///   - port: 默认 ``BuzzLinkConstants/loopbackPort``（UInt16 全域合法，回退仅防御）
    public init(host: String = "127.0.0.1", port: UInt16 = BuzzLinkConstants.loopbackPort) {
        self.host = NWEndpoint.Host(host)
        self.port = NWEndpoint.Port(rawValue: port) ?? 0
        let states = AsyncStream<BuzzLinkTransportState>.makeStream()
        self.stateStream = states.stream
        self.stateContinuation = states.continuation
        let inbound = AsyncStream<Data>.makeStream()
        self.inbound = inbound.stream
        self.inboundContinuation = inbound.continuation
    }

    deinit {
        let connection = self.connection
        queue.async {
            connection?.cancel()
        }
    }

    // MARK: - BuzzLinkTransport

    public func connect() async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, any Error>) in
            queue.async {
                self.startConnection(continuation: continuation)
            }
        }
    }

    public func disconnect() {
        queue.async {
            self.connection?.cancel()
        }
    }

    public func send(_ bytes: Data) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, any Error>) in
            queue.async {
                guard let connection = self.connection else {
                    continuation.resume(throwing: BuzzLinkError.notConnected)
                    return
                }
                connection.send(content: bytes, completion: .contentProcessed { error in
                    if let error {
                        continuation.resume(throwing: BuzzLinkError.transport(error.localizedDescription))
                    } else {
                        continuation.resume()
                    }
                })
            }
        }
    }

    // MARK: - 私有：连接管理（串行队列上下文）

    private func startConnection(continuation: CheckedContinuation<Void, any Error>) {
        // 重连：丢弃旧连接
        connection?.cancel()

        let parameters = NWParameters.tcp
        parameters.requiredInterfaceType = .loopback
        let connection = NWConnection(host: host, port: port, using: parameters)
        self.connection = connection

        connection.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .setup:
                break
            case .preparing:
                self.stateContinuation.yield(.connecting)
            case .ready:
                self.stateContinuation.yield(.ready)
                continuation.resume()
                self.receiveNext(on: connection)
            case .failed(let error):
                self.stateContinuation.yield(.disconnected)
                continuation.resume(throwing: BuzzLinkError.transport(error.localizedDescription))
            case .cancelled:
                self.stateContinuation.yield(.disconnected)
            default:
                break
            }
        }
        connection.start(queue: queue)
    }

    private func receiveNext(on connection: NWConnection) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 65_536) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            if let data, !data.isEmpty {
                self.inboundContinuation.yield(data)
            }
            if isComplete || error != nil {
                self.stateContinuation.yield(.disconnected)
                return
            }
            self.receiveNext(on: connection)
        }
    }
}
