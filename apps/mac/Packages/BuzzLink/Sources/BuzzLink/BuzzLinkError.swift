import BuzzLinkCore
import Foundation

/// BuzzLink Central 错误（命令超时/协议错误/status 与 JSON 错误码映射，§5.1/§9）。
public enum BuzzLinkError: Error, Sendable, Equatable {

    // MARK: - 本地状态/传输

    /// 当前客户端状态不允许该操作
    case invalidState
    /// 未连接（idle/disconnected 状态下执行命令）
    case notConnected
    /// 参数非法（如 offset 为负、文件 ID 超限）
    case invalidParameters
    /// 传输层错误（携带原始描述）
    case transport(String)
    /// BLE 不可用（未授权/未开启/不支持）
    case bluetoothUnavailable
    /// 连接已断开（未完成命令全部作废，§7）
    case disconnected
    /// 调用方任务被取消
    case cancelled

    // MARK: - 超时（§3.4）

    /// 命令响应超时（5 s）
    case commandTimeout(session: UInt8)
    /// 下载中相邻 DL_CHUNK 间隔超时（10 s）
    case chunkTimeout(fileID: String)

    // MARK: - 对端错误（status 字节 / JSON code，§5.1/§9）

    /// 4009 busy：设备忙（如推流中又收到 BEGIN）
    case busy
    /// 4000 bad_request：帧/参数非法
    case badRequest
    /// 4003 forbidden
    case forbidden
    /// 4004 not_found
    case notFound
    /// status=2 aborted：下载被中止
    case aborted
    /// 4500 storage_error
    case storage
    /// 4999 internal 或未知错误码（携带描述）
    case internalFailure(String)

    // MARK: - 协议与验收

    /// 响应帧结构非法（type/字段长度不符）
    case protocolViolation(String)
    /// 重组后文件大小与 BEGIN 声明不符
    case sizeMismatch(expected: Int, actual: Int)
    /// 文件 CRC16 不等（三处 CRC 裁决失败，§6.3）
    case crcMismatch(expected: UInt16, actual: UInt16)
    /// 整文件重试次数耗尽仍失败（§7）
    indirect case downloadRetryExhausted(fileID: String, attempts: Int, lastError: BuzzLinkError)
}

extension BuzzLinkError {

    /// DL_BEGIN_RESP/DL_END_RESP status 字节 → 错误（§5.1）。
    /// 调用方保证仅在 status != ok 时使用。
    static func mapStatus(_ status: BuzzLinkStatus) -> BuzzLinkError {
        switch status {
        case .ok: return .internalFailure("ok 状态不是错误")
        case .notFound: return .notFound
        case .aborted: return .aborted
        case .busy: return .busy
        case .badRequest: return .badRequest
        case .storageError: return .storage
        case .internalError: return .internalFailure("设备内部错误（status=6）")
        }
    }

    /// ERROR 帧/错误 JSON 的 code → 错误（§9）。
    static func mapErrorCode(_ code: Int, message: String) -> BuzzLinkError {
        switch code {
        case 4_000: return .badRequest
        case 4_003: return .forbidden
        case 4_004: return .notFound
        case 4_009: return .busy
        case 4_130: return .badRequest
        case 4_500: return .storage
        case 4_999: return .internalFailure(message)
        default: return .internalFailure("未知错误码 \(code)：\(message)")
        }
    }

    /// 该错误是否允许按 §7 整文件重试（DL_ABORT 清理后重新下载）。
    /// 下载流内的命令超时（BEGIN/END）同属 §7"超时即失败可重发"——设备可能正处于
    /// 故障静默，重试一趟是规范 recovery 路径。
    var allowsDownloadRetry: Bool {
        switch self {
        case .chunkTimeout, .crcMismatch, .sizeMismatch, .commandTimeout:
            return true
        default:
            return false
        }
    }
}

extension BuzzLinkError: LocalizedError {
    public var errorDescription: String? {
        switch self {
        case .invalidState:
            return "当前状态不允许该操作"
        case .notConnected:
            return "设备未连接"
        case .invalidParameters:
            return "参数非法"
        case .transport(let message):
            return "传输层错误：\(message)"
        case .bluetoothUnavailable:
            return "蓝牙不可用（未授权/未开启/不支持）"
        case .disconnected:
            return "连接已断开"
        case .cancelled:
            return "操作已取消"
        case .commandTimeout(let session):
            return String(format: "命令响应超时（session=0x%02X）", session)
        case .chunkTimeout(let fileID):
            return "下载块超时：\(fileID)"
        case .busy:
            return "设备忙（4009 busy）"
        case .badRequest:
            return "请求非法（4000 bad_request）"
        case .forbidden:
            return "操作被禁止（4003 forbidden）"
        case .notFound:
            return "文件不存在（4004 not_found）"
        case .aborted:
            return "下载已中止（status=2）"
        case .storage:
            return "设备存储错误（4500 storage_error）"
        case .internalFailure(let message):
            return "设备内部错误：\(message)"
        case .protocolViolation(let message):
            return "协议错误：\(message)"
        case .sizeMismatch(let expected, let actual):
            return "文件大小不符：期望 \(expected) 字节，实际 \(actual) 字节"
        case .crcMismatch(let expected, let actual):
            return String(format: "文件 CRC 校验失败：期望 0x%04X，实际 0x%04X", expected, actual)
        case .downloadRetryExhausted(let fileID, let attempts, let lastError):
            return "下载 \(fileID) 重试 \(attempts) 次仍失败：\(lastError.localizedDescription)"
        }
    }
}
