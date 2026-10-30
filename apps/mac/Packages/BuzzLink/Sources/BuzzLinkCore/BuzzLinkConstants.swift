import Foundation

/// BuzzLink 协议常量（BUZZLINK.md §2/§3/§5/§6 的唯一代码来源）。
///
/// 修改协议常量 = 先改 BUZZLINK.md 与测试向量，再改这里。
public enum BuzzLinkConstants {

    // MARK: - 帧结构（§3.1）

    /// 帧魔数第 1 字节
    public static let magicByte0: UInt8 = 0xBE
    /// 帧魔数第 2 字节
    public static let magicByte1: UInt8 = 0xEC
    /// 协议版本（frame version，§10）
    public static let version: UInt8 = 0x01
    /// 帧头固定长度
    public static let headerLength = 10
    /// 帧尾 CRC 长度
    public static let crcLength = 2
    /// 整帧开销 = 头 + CRC
    public static let frameOverhead = headerLength + crcLength
    /// flags bit0：FRAG 分片标记（§3.3）
    public static let flagFragment: UInt8 = 0x01

    // MARK: - 传输（§2.1/§3.3）

    /// Central 应请求的 MTU
    public static let defaultMTU = 247
    /// 最低可接受 MTU
    public static let minimumMTU = 185
    /// BLE ATT 层开销（MTU 减出 ATT 头后才是 GATT 有效载荷）
    public static let attOverhead = 3
    /// 单帧 payload 上限 = MTU - ATT 开销 - 帧开销；MTU 247 时为 232
    public static func maxPayloadPerFrame(mtu: Int = defaultMTU) -> Int {
        mtu - attOverhead - frameOverhead
    }
    /// 分片帧 payload 中 fragIndex(2) + fragCount(2) 的头长
    public static let fragmentHeaderLength = 4
    /// 单个分片可携带的数据上限；MTU 247 时为 228
    public static func maxFragmentData(mtu: Int = defaultMTU) -> Int {
        maxPayloadPerFrame(mtu: mtu) - fragmentHeaderLength
    }
    /// 重组后消息总上限 64 KB
    public static let maxMessageSize = 64 * 1024

    // MARK: - 文件下载（§6.2）

    /// 设备声明的默认块数据长度
    public static let defaultChunkDataLength = 224
    /// 设备可声明的块数据长度上限
    public static let maxChunkDataLength = 230
    /// 文件 CRC 失败时整文件重下次数上限（§7）
    public static let maxFileDownloadRetries = 2

    // MARK: - 超时（§3.4）

    /// 命令响应超时（秒）
    public static let commandTimeout: TimeInterval = 5
    /// 下载中相邻 DL_CHUNK 间隔超时（秒）
    public static let chunkIntervalTimeout: TimeInterval = 10

    // MARK: - 数据模型（§5.2）

    /// 文件 ID 字节数上限
    public static let maxFileIDLength = 32
    /// 列表每页条数上限
    public static let listPageLimit = 16

    // MARK: - GATT（§2.1/§2.2）

    /// BuzzLink Service UUID（字符串形式；CBUUID 由 Central 层包装）
    public static let serviceUUIDString = "0E242009-7EF4-4AE0-B3B0-E3A59E58D47C"
    /// Cmd Write 特征 UUID（Central→Device，Write Without Response）
    public static let cmdWriteUUIDString = "A2268D42-7024-430F-B059-9A156436973D"
    /// Event Notify 特征 UUID（Device→Central）
    public static let eventNotifyUUIDString = "C22080F5-0E96-4823-8909-1DD2CE427D2D"
    /// 广播名前缀（`BCD-XXXX`）
    public static let deviceNamePrefix = "BCD-"

    // MARK: - Loopback 传输（TESTING.md §4 同机开发通道；非空口协议常量）

    /// BuzzLinkSim 的同机回环 TCP 监听端口（仅开发/测试用途）。
    /// 背景：macOS 同机 Central 无法发现本机 Peripheral 广播，同机联调走此通道，
    /// 字节流与空口完全同构（§3.3 字节流重组天然适配流式 socket）。
    public static let loopbackPort: UInt16 = 40404
}

/// 帧类型（BUZZLINK.md §5.1）。
///
/// 0x36 为预留的 DL_NACK（选择性重传演进方向），P0 不实现。
public enum BuzzLinkFrameType: UInt8, Sendable, CaseIterable {
    case ping = 0x01
    case pong = 0x02
    case info = 0x03
    case infoGet = 0x04
    case listReq = 0x10
    case listResp = 0x11
    case fileInfoReq = 0x20
    case fileInfoResp = 0x21
    case dlBeginReq = 0x30
    case dlBeginResp = 0x31
    case dlChunk = 0x32
    case dlEndReq = 0x33
    case dlEndResp = 0x34
    case dlAbort = 0x35
    case fileDeleteReq = 0x40
    case fileDeleteResp = 0x41
    case timeSetReq = 0x42   // v0.2 §8.4：C→D 对时
    case timeSetResp = 0x43  // v0.2 §8.4：D→C
    case error = 0x7F
}

/// 通道（BUZZLINK.md §4）。
public enum BuzzLinkChannel: UInt16, Sendable {
    /// 链路（PING/PONG）
    case link = 0x0000
    /// 控制（JSON）
    case control = 0x0001
    /// 文件（二进制）
    case file = 0x0002
}

/// status 字节（BUZZLINK.md §5.1）。
public enum BuzzLinkStatus: UInt8, Sendable {
    case ok = 0
    case notFound = 1
    case aborted = 2
    case busy = 3
    case badRequest = 4
    case storageError = 5
    case internalError = 6
}
