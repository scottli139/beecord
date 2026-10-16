import BuzzLinkCore
import CoreBluetooth
import Foundation
import os

/// BLE 传输（Central 侧）：扫描 → 连接 → 服务/特征发现 → 订阅 Notify → write-without-response。
///
/// 薄壳：仅做 GATT IO 与状态桥接；协议行为（session/超时/重组/CRC）全部在
/// ``BuzzLinkClient``。delegate 回调在主队列，按项目惯例 `nonisolated` +
/// `MainActor.assumeIsolated` hop 回主 actor。写出采用 `peripheralIsReady` 背压续传，
/// 应用层节流策略由上层决定（studio-tech-design §4.2）。
///
/// Apple 平台 ATT MTU 由系统协商，无法直接请求 247；本实现按
/// `maximumWriteValueLength(for: .withoutResponse)` 观察实际可写长度。
@MainActor
public final class BLETransport: NSObject {

    public let stateStream: AsyncStream<BuzzLinkTransportState>
    public let inbound: AsyncStream<Data>

    /// 扫描超时（找不到设备即失败，避免无限等待）
    public var scanTimeout: TimeInterval = 10

    private let logger = Logger(subsystem: "com.beeuc.beecord.buzzlink", category: "transport")
    private let stateContinuation: AsyncStream<BuzzLinkTransportState>.Continuation
    private let inboundContinuation: AsyncStream<Data>.Continuation

    private var manager: CBCentralManager?
    private var peripheral: CBPeripheral?
    private var writeCharacteristic: CBCharacteristic?
    private var notifyCharacteristic: CBCharacteristic?
    private var connectContinuation: CheckedContinuation<Void, any Error>?
    private var pendingWrites: [Data] = []

    /// 已产出过 `.disconnected`（连接失败/断连均收敛到该终态，客户端据此清理）
    private var didDisconnect = false

    public override init() {
        let states = AsyncStream<BuzzLinkTransportState>.makeStream()
        self.stateStream = states.stream
        self.stateContinuation = states.continuation
        let inbound = AsyncStream<Data>.makeStream()
        self.inbound = inbound.stream
        self.inboundContinuation = inbound.continuation
    }

    // MARK: - BuzzLinkTransport（同步隔离实现满足 async 协议要求）

    public func connect() async throws {
        try await withCheckedThrowingContinuation { continuation in
            self.connectContinuation = continuation
            self.didDisconnect = false
            if let manager {
                beginScanOrThrow(manager: manager)
            } else {
                manager = CBCentralManager(delegate: self, queue: .main)
            }
            self.armScanTimeout()
        }
    }

    public func disconnect() {
        pendingWrites.removeAll()
        if let peripheral {
            manager?.cancelPeripheralConnection(peripheral)
        }
        manager?.stopScan()
        emitDisconnectedIfNeeded(force: true)
    }

    public func send(_ bytes: Data) throws {
        guard let peripheral, writeCharacteristic != nil else {
            throw BuzzLinkError.notConnected
        }
        pendingWrites.append(bytes)
        drainPendingWrites(peripheral: peripheral)
    }

    // MARK: - 私有：连接流程（主 actor）

    private func beginScanOrThrow(manager: CBCentralManager) {
        guard manager.state == .poweredOn else {
            failConnect(BuzzLinkError.bluetoothUnavailable)
            return
        }
        stateContinuation.yield(.scanning)
        manager.scanForPeripherals(
            withServices: [CBUUID(string: BuzzLinkConstants.serviceUUIDString)],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
    }

    /// 扫描超时：无设备即失败（调用方决定是否重试/换通道）
    private func armScanTimeout() {
        let timeout = scanTimeout
        Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(max(1, Int(timeout * 1000))))
            guard let self, self.connectContinuation != nil else { return }
            self.manager?.stopScan()
            self.failConnect(BuzzLinkError.transport("扫描超时（\(timeout)s 内未发现设备）"))
        }
    }

    private func failConnect(_ error: BuzzLinkError) {
        let continuation = connectContinuation
        connectContinuation = nil
        continuation?.resume(throwing: error)
        emitDisconnectedIfNeeded()
    }

    private func emitDisconnectedIfNeeded(force: Bool = false) {
        guard force || !didDisconnect else { return }
        didDisconnect = true
        stateContinuation.yield(.disconnected)
    }

    /// write-without-response 背压续传（GATT 队列满时 peripheralIsReady 再推）
    private func drainPendingWrites(peripheral: CBPeripheral) {
        guard let writeCharacteristic else { return }
        while !pendingWrites.isEmpty {
            peripheral.writeValue(pendingWrites[0], for: writeCharacteristic, type: .withoutResponse)
            pendingWrites.removeFirst()
            if !peripheral.canSendWriteWithoutResponse {
                // GATT 队列已满，等 peripheralIsReady 回调续传
                return
            }
        }
    }
}

// MARK: - BuzzLinkTransport 遵从
//
// 类为 @MainActor 隔离，协议要求非隔离：以 @preconcurrency 声明遵从
// （隔离跃迁在运行期受主队列保证——delegate 回调与公共入口均经主队列派发）。

extension BLETransport: @preconcurrency BuzzLinkTransport {
}

// MARK: - CBCentralManagerDelegate（主队列回调，统一 hop 回 MainActor）
//
// delegate 方法必须为 nonisolated（@objc 协议要求）；本类 manager 以 queue: .main 创建，
// 回调必定已在主队列，assumeIsolated 语义合法。CB 对象非 Sendable、Swift 6 区域隔离
// 无法证明其归属，经 ``MainActorBox`` 显式豁免转移。

extension BLETransport: CBCentralManagerDelegate {

    nonisolated public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let box = MainActorBox(value: (central, central.state))
        MainActor.assumeIsolated {
            let (central, state) = box.value
            logger.info("蓝牙状态变化：\(state.rawValue)")
            if state == .poweredOn, connectContinuation != nil {
                beginScanOrThrow(manager: central)
            } else if state != .poweredOn, connectContinuation != nil {
                failConnect(.bluetoothUnavailable)
            }
        }
    }

    nonisolated public func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        let box = MainActorBox(value: (central, peripheral))
        MainActor.assumeIsolated {
            let (central, peripheral) = box.value
            // 以 Service UUID 过滤为准（§2.2），名称仅作展示
            central.stopScan()
            self.peripheral = peripheral
            peripheral.delegate = self
            stateContinuation.yield(.connecting)
            central.connect(peripheral, options: nil)
        }
    }

    nonisolated public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        let box = MainActorBox(value: peripheral)
        MainActor.assumeIsolated {
            box.value.discoverServices([CBUUID(string: BuzzLinkConstants.serviceUUIDString)])
        }
    }

    nonisolated public func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: (any Error)?
    ) {
        let message = error?.localizedDescription ?? "未知原因"
        MainActor.assumeIsolated {
            failConnect(.transport("连接失败：\(message)"))
        }
    }

    nonisolated public func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: (any Error)?
    ) {
        MainActor.assumeIsolated {
            self.peripheral = nil
            writeCharacteristic = nil
            notifyCharacteristic = nil
            pendingWrites.removeAll()
            // 若 connect 仍在等待（订阅前断连），按失败收尾
            if let continuation = connectContinuation {
                connectContinuation = nil
                continuation.resume(throwing: BuzzLinkError.disconnected)
            }
            emitDisconnectedIfNeeded()
        }
    }
}

// MARK: - CBPeripheralDelegate（主队列回调，统一 hop 回 MainActor）

extension BLETransport: CBPeripheralDelegate {

    nonisolated public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: (any Error)?) {
        let message = error?.localizedDescription
        let box = MainActorBox(value: peripheral)
        MainActor.assumeIsolated {
            let peripheral = box.value
            guard message == nil,
                  let service = peripheral.services?.first(
                      where: { $0.uuid == CBUUID(string: BuzzLinkConstants.serviceUUIDString) }
                  ) else {
                failConnect(.transport("服务发现失败：\(message ?? "未找到 BuzzLink 服务")"))
                return
            }
            peripheral.discoverCharacteristics(
                [
                    CBUUID(string: BuzzLinkConstants.cmdWriteUUIDString),
                    CBUUID(string: BuzzLinkConstants.eventNotifyUUIDString)
                ],
                for: service
            )
        }
    }

    nonisolated public func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: (any Error)?
    ) {
        let message = error?.localizedDescription
        let box = MainActorBox(value: (peripheral, service))
        MainActor.assumeIsolated {
            let (peripheral, service) = box.value
            guard message == nil, let characteristics = service.characteristics else {
                failConnect(.transport("特征发现失败：\(message ?? "无特征")"))
                return
            }
            for characteristic in characteristics {
                if characteristic.uuid == CBUUID(string: BuzzLinkConstants.cmdWriteUUIDString) {
                    writeCharacteristic = characteristic
                } else if characteristic.uuid == CBUUID(string: BuzzLinkConstants.eventNotifyUUIDString) {
                    notifyCharacteristic = characteristic
                    peripheral.setNotifyValue(true, for: characteristic)
                }
            }
            guard writeCharacteristic != nil, notifyCharacteristic != nil else {
                failConnect(.transport("BuzzLink 特征不全"))
                return
            }
            stateContinuation.yield(.ready)
            let continuation = connectContinuation
            connectContinuation = nil
            continuation?.resume()
        }
    }

    nonisolated public func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: (any Error)?) {
        // Data 为 Sendable，直接捕获（与 SimPeripheral 同路）
        let value = error == nil ? characteristic.value : nil
        MainActor.assumeIsolated {
            guard let value, !value.isEmpty else { return }
            inboundContinuation.yield(value)
        }
    }

    nonisolated public func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        let box = MainActorBox(value: peripheral)
        MainActor.assumeIsolated {
            drainPendingWrites(peripheral: box.value)
        }
    }
}

// MARK: - 主队列 → MainActor 转移盒

/// delegate 回调（已在主队列）向 MainActor 闭包转移非 Sendable 值的人工安全通道：
/// 安全依据是 manager 以 `queue: .main` 创建、回调线程归属确定；
/// Swift 6 区域隔离无法自动证明，故以 `@unchecked Sendable` 显式豁免。
private struct MainActorBox<Value>: @unchecked Sendable {
    let value: Value
}
