import Foundation

/// 传输层状态（Transport 视角；客户端状态机见 ``BuzzLinkClientState``）。
public enum BuzzLinkTransportState: Sendable, Equatable {
    case idle
    /// BLE 扫描中（loopback 无此阶段，由实现直接略过）
    case scanning
    case connecting
    /// 传输就绪：可 `send`/`receive`（服务发现/订阅/MTU 协商均已完成）
    case ready
    case disconnected
}

/// BuzzLink 传输抽象（BUZZLINK.md §2：GATT write/notify 字节流；
/// TESTING.md §4：loopback TCP 与空口字节流同构）。
///
/// 实现契约：
/// - `connect()` 返回时传输必须已就绪（发现服务/订阅 Notify 完成），此后可 `send`；
/// - `inbound` 产出收到的字节流**片段**：一次 GATT notify 或一次 TCP 读可能携带任意
///   切分/合并的数据，接收方按 §3.3 字节流重组，本层不做帧解析；
/// - `stateStream` 持续产出状态变化；`disconnect()` 后必须发出 `.disconnected` 终态；
/// - 支持断线重连：同一实例上再次调用 `connect()`。
public protocol BuzzLinkTransport: Sendable {

    /// 状态变化流
    var stateStream: AsyncStream<BuzzLinkTransportState> { get }

    /// 入站字节流片段
    var inbound: AsyncStream<Data> { get }

    /// 建立连接；返回时传输已就绪
    func connect() async throws

    /// 断开（触发 `stateStream` 发出 `.disconnected`）
    func disconnect()

    /// 发送一帧已编码的线上字节
    func send(_ bytes: Data) async throws
}
