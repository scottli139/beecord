import BuzzLinkCore
import CoreBluetooth
import Foundation
import os

/// BLE 传输（Central 侧）：扫描 → 连接 → 服务/特征发现 → 订阅 Notify → write-without-response。
///
/// 薄壳：仅做 GATT IO 与状态桥接；协议行为（session/超时/重组/CRC）全部在
/// ``BuzzLinkClient``。线程模型与 ``LoopbackTransport`` 同构：非隔离类 +
/// 专用串行队列（`bleTransport`）。CBCentralManager 以该队列创建，delegate 回调
/// 与公共入口均在同一条队列上执行/派发，全部可变状态限定于该队列，故声明
/// `@unchecked Sendable`——从任意 executor（client actor、主队列等）调用
/// connect/disconnect/send 均无隔离跃迁与 executor 断言。写出采用
/// `canSendWriteWithoutResponse` 背压续传（队列满时等 peripheralIsReady 回调再推），
/// 应用层节流策略由上层决定（studio-tech-design §4.2）。
///
/// Apple 平台 ATT MTU 由系统协商，无法直接请求 247；本实现按系统实际接受的
/// write-without-response 推流，可写长度以 CoreBluetooth 背压信号为准。
public final class BLETransport: NSObject, BuzzLinkTransport, @unchecked Sendable {

    public let stateStream: AsyncStream<BuzzLinkTransportState>
    public let inbound: AsyncStream<Data>

    /// 扫描超时（找不到设备即失败，避免无限等待）；仅在 `connect()` 派发至串行队列后读取
    public var scanTimeout: TimeInterval = 10

    private let queue = DispatchQueue(label: "com.beeuc.beecord.buzzlink.bleTransport")
    private let logger = Logger(subsystem: "com.beeuc.beecord.buzzlink", category: "transport")
    private let stateContinuation: AsyncStream<BuzzLinkTransportState>.Continuation
    private let inboundContinuation: AsyncStream<Data>.Continuation

    /// 以下成员仅在串行队列上访问（delegate 回调与公共入口同队列，天然互斥）
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

    // MARK: - BuzzLinkTransport

    public func connect() async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, any Error>) in
            queue.async {
                self.connectContinuation = continuation
                self.didDisconnect = false
                if let manager = self.manager {
                    self.beginScanOrThrow(manager: manager)
                } else {
                    self.manager = CBCentralManager(delegate: self, queue: self.queue)
                }
                self.armScanTimeout()
            }
        }
    }

    public func disconnect() {
        queue.async {
            self.pendingWrites.removeAll()
            if let peripheral = self.peripheral {
                self.manager?.cancelPeripheralConnection(peripheral)
            }
            self.manager?.stopScan()
            self.emitDisconnectedIfNeeded(force: true)
        }
    }

    public func send(_ bytes: Data) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, any Error>) in
            queue.async {
                guard let peripheral = self.peripheral, self.writeCharacteristic != nil else {
                    continuation.resume(throwing: BuzzLinkError.notConnected)
                    return
                }
                self.pendingWrites.append(bytes)
                self.drainPendingWrites(peripheral: peripheral)
                continuation.resume()
            }
        }
    }

    // MARK: - 私有：连接流程（串行队列上下文）

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
        queue.asyncAfter(deadline: .now() + timeout) { [weak self] in
            guard let self, self.connectContinuation != nil else { return }
            self.manager?.stopScan()
            self.failConnect(.transport("扫描超时（\(timeout)s 内未发现设备）"))
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

    /// write-without-response 背压续传（GATT 队列满时 peripheralIsReady 回调再推）
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

// MARK: - CBCentralManagerDelegate
//
// manager 以串行队列创建，回调必定在该队列上派发，与公共入口同队列，
// 直接访问可变状态（无需锁或 actor hop）。

extension BLETransport: CBCentralManagerDelegate {

    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        logger.info("蓝牙状态变化：\(central.state.rawValue)")
        if central.state == .poweredOn, connectContinuation != nil {
            beginScanOrThrow(manager: central)
        } else if central.state != .poweredOn, connectContinuation != nil {
            failConnect(.bluetoothUnavailable)
        }
    }

    public func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        // 以 Service UUID 过滤为准（§2.2），名称仅作展示
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        stateContinuation.yield(.connecting)
        central.connect(peripheral, options: nil)
    }

    public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([CBUUID(string: BuzzLinkConstants.serviceUUIDString)])
    }

    public func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: (any Error)?
    ) {
        let message = error?.localizedDescription ?? "未知原因"
        failConnect(.transport("连接失败：\(message)"))
    }

    public func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: (any Error)?
    ) {
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

// MARK: - CBPeripheralDelegate
//
// 回调经 central.connect 派发到 manager 同一串行队列，线程模型同上。

extension BLETransport: CBPeripheralDelegate {

    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: (any Error)?) {
        guard error == nil,
              let service = peripheral.services?.first(
                  where: { $0.uuid == CBUUID(string: BuzzLinkConstants.serviceUUIDString) }
              ) else {
            failConnect(.transport("服务发现失败：\(error?.localizedDescription ?? "未找到 BuzzLink 服务")"))
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

    public func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: (any Error)?
    ) {
        guard error == nil, let characteristics = service.characteristics else {
            failConnect(.transport("特征发现失败：\(error?.localizedDescription ?? "无特征")"))
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

    public func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: (any Error)?
    ) {
        // Data 为 Sendable；按 notify 片段原样上抛，帧重组在客户端（§3.3）
        guard error == nil, let value = characteristic.value, !value.isEmpty else { return }
        inboundContinuation.yield(value)
    }

    public func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        drainPendingWrites(peripheral: peripheral)
    }
}
