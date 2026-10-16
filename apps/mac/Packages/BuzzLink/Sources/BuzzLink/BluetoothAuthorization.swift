import CoreBluetooth
import Foundation

/// 蓝牙授权/可用状态（上层权限面罩文案细分用）。
public enum BuzzLinkBluetoothStatus: Sendable, Equatable {
    /// 尚未请求授权（首次 BLE 连接由系统弹窗询问）
    case notDetermined
    /// 用户已拒绝或受系统限制
    case denied
    /// 蓝牙已关闭（或正在重置）
    case poweredOff
    /// 已授权且可用
    case ready
}

/// 蓝牙状态查询。
///
/// `currentStatus()` 只读 `CBManager.authorization` 类属性，**不实例化
/// CBCentralManager**——避免在 UI 探测阶段提前触发系统授权弹窗（弹窗时机
/// 留给真实的 ``BLETransport/connect()``）。因此它无法产出 `.poweredOff`
/// （无线电电源状态只有 manager 实例才能观察）：上层在 BLE 连接抛
/// ``BuzzLinkError/bluetoothUnavailable`` 且 `currentStatus()` 为 `.ready` 时，
/// 可推断为蓝牙未开启/不支持，按 `.poweredOff` 呈现。
public enum BuzzLinkBluetooth {

    /// 当前授权状态（`CBManager.authorization`）
    public static func currentStatus() -> BuzzLinkBluetoothStatus {
        switch CBManager.authorization {
        case .allowedAlways:
            return .ready
        case .denied, .restricted:
            return .denied
        case .notDetermined:
            return .notDetermined
        @unknown default:
            return .notDetermined
        }
    }
}
