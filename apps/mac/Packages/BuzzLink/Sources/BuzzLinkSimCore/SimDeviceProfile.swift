import Foundation

/// 虚拟设备档案（BUZZLINK.md §8.1 INFO 字段来源）。
public struct SimDeviceProfile: Equatable, Sendable {
    /// 设备型号
    public var model: String
    /// 固件版本
    public var fwVersion: String
    /// 广播名（`BCD-XXXX`，§2.2）
    public var deviceName: String
    /// 电量百分比（0–100）
    public var battery: Int
    /// 是否充电中
    public var isCharging: Bool
    /// 存储总容量（字节）
    public var storageTotal: UInt64
    /// 存储剩余（字节）
    public var storageFree: UInt64

    public init(
        model: String = "beecord-sim",
        fwVersion: String = "0.1.0",
        deviceName: String = "BCD-DEMO",
        battery: Int = 87,
        isCharging: Bool = false,
        storageTotal: UInt64 = 31_000_000,
        storageFree: UInt64 = 12_345_678
    ) {
        self.model = model
        self.fwVersion = fwVersion
        self.deviceName = deviceName
        self.battery = battery
        self.isCharging = isCharging
        self.storageTotal = storageTotal
        self.storageFree = storageFree
    }
}
