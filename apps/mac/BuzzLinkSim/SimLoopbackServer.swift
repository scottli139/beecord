import Foundation
import Network
import os

/// Loopback 传输服务器（TESTING.md §4 同机开发/测试通道）。
///
/// 监听 127.0.0.1:``BuzzLinkConstants/loopbackPort``，承载与 BLE 空口完全同构的
/// BuzzLink 字节流（§3.3 字节流重组天然适配流式 socket）。背景：macOS 同机
/// Central 无法发现本机 Peripheral 广播，同机联调与 UITests 走此通道。
@MainActor
final class SimLoopbackServer {

    /// 收到某连接的字节流片段（参数：连接标识、字节）
    var onReceive: ((ObjectIdentifier, Data) -> Void)?
    /// 连接数变化
    var onConnectionsChanged: ((Int) -> Void)?
    /// 系统事件（监听失败等），供 UI 日志
    var onEvent: ((String) -> Void)?

    private(set) var isListening = false
    private(set) var connectionCount = 0

    private let logger = Logger(subsystem: "com.beeuc.beecord.sim", category: "loopback")
    private var listener: NWListener?
    private var connections: [ObjectIdentifier: NWConnection] = [:]

    /// 启动监听（重复调用幂等）
    func start(port: UInt16) {
        guard listener == nil else { return }
        guard let endpointPort = NWEndpoint.Port(rawValue: port) else {
            onEvent?("loopback 端口非法：\(port)")
            return
        }
        do {
            let parameters = NWParameters.tcp
            parameters.requiredInterfaceType = .loopback
            let listener = try NWListener(using: parameters, on: endpointPort)
            self.listener = listener
            listener.newConnectionHandler = { [weak self] connection in
                MainActor.assumeIsolated {
                    self?.accept(connection)
                }
            }
            listener.stateUpdateHandler = { [weak self] state in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    switch state {
                    case .ready:
                        self.isListening = true
                        self.onEvent?("loopback 监听中：127.0.0.1:\(port)")
                    case .failed(let error):
                        self.isListening = false
                        self.logger.error("loopback 监听失败：\(error.localizedDescription)")
                        self.onEvent?("loopback 监听失败：\(error.localizedDescription)")
                    case .cancelled:
                        self.isListening = false
                    default:
                        break
                    }
                }
            }
            listener.start(queue: .main)
        } catch {
            logger.error("loopback 监听创建失败：\(error.localizedDescription)")
            onEvent?("loopback 监听创建失败：\(error.localizedDescription)")
        }
    }

    func stop() {
        listener?.cancel()
        listener = nil
        for connection in connections.values {
            connection.cancel()
        }
        connections.removeAll()
        connectionCount = 0
        isListening = false
    }

    /// 向全部已连接客户端广播字节（设备语义：等同 BLE notify 给所有订阅者）
    func broadcast(_ bytes: Data) {
        for connection in connections.values {
            connection.send(content: bytes, completion: .idempotent)
        }
    }

    /// 断开指定连接（连接级会话重置）
    func drop(_ id: ObjectIdentifier) {
        connections[id]?.cancel()
        connections.removeValue(forKey: id)
        connectionCount = connections.count
    }

    // MARK: - 私有

    private func accept(_ connection: NWConnection) {
        let id = ObjectIdentifier(connection)
        connections[id] = connection
        connectionCount = connections.count
        onConnectionsChanged?(connectionCount)
        onEvent?("loopback 客户端已连接（共 \(connectionCount) 个）")

        connection.stateUpdateHandler = { [weak self] state in
            MainActor.assumeIsolated {
                guard let self else { return }
                switch state {
                case .failed, .cancelled:
                    self.connections.removeValue(forKey: id)
                    self.connectionCount = self.connections.count
                    self.onConnectionsChanged?(self.connectionCount)
                default:
                    break
                }
            }
        }
        connection.start(queue: .main)
        receiveLoop(connection, id: id)
    }

    private func receiveLoop(_ connection: NWConnection, id: ObjectIdentifier) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 65_536) { [weak self] data, _, isComplete, error in
            MainActor.assumeIsolated {
                guard let self else { return }
                if let data, !data.isEmpty {
                    self.onReceive?(id, data)
                }
                if isComplete || error != nil {
                    connection.cancel()
                    self.connections.removeValue(forKey: id)
                    self.connectionCount = self.connections.count
                    self.onConnectionsChanged?(self.connectionCount)
                    return
                }
                self.receiveLoop(connection, id: id)
            }
        }
    }
}
