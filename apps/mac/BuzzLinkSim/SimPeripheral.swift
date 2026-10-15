import BuzzLinkCore
import CoreBluetooth
import Foundation
import os

/// BuzzLink 虚拟设备的 BLE 外设壳：只做 GATT IO（广播/订阅/收发字节流），
/// 协议行为全部在 BuzzLinkSimCore 的 `SimEngine`。
///
/// 回调队列为主队列，全部成员限定主 actor 访问。
@MainActor
final class SimPeripheral: NSObject {

    /// 收到 Central 的 Write Without Response 字节（可能任意切分/合并，§3.3）
    var onWrite: ((Data) -> Void)?
    /// Central 订阅/退订 Notify 特征
    var onSubscribed: (() -> Void)?
    var onUnsubscribed: (() -> Void)?
    /// 蓝牙状态变化
    var onStateChange: ((CBManagerState) -> Void)?
    /// 系统级事件（广播失败等），供 UI 日志
    var onEvent: ((String) -> Void)?

    private(set) var state: CBManagerState = .unknown
    private(set) var subscribedCentralCount = 0

    private let logger = Logger(subsystem: "com.beeuc.beecord.sim", category: "peripheral")
    private var manager: CBPeripheralManager?
    private var notifyCharacteristic: CBMutableCharacteristic?
    private var pendingOutbound: [Data] = []

    /// 创建 CBPeripheralManager（触发系统蓝牙授权与状态回调）
    func start() {
        guard manager == nil else { return }
        manager = CBPeripheralManager(delegate: self, queue: nil)
    }

    /// 开关广播（§2.2：AD 含 Service UUID + Complete Local Name）
    func setAdvertising(_ wanted: Bool, deviceName: String) {
        guard let manager, manager.state == .poweredOn else { return }
        if wanted {
            // §2.2：AD 含 128-bit Service UUID + Complete Local Name
            manager.startAdvertising([
                CBAdvertisementDataServiceUUIDsKey: [CBUUID(string: BuzzLinkConstants.serviceUUIDString)],
                CBAdvertisementDataLocalNameKey: deviceName
            ])
        } else {
            manager.stopAdvertising()
        }
    }

    /// 向全部订阅 Central 推送一帧字节；ATT 队列满时暂存，待可写续传
    func notify(_ bytes: Data) {
        pendingOutbound.append(bytes)
        drainPending()
    }

    /// 丢弃未发出的字节（断连/会话重置时调用）
    func flushPending() {
        pendingOutbound.removeAll()
    }

    private func drainPending() {
        guard let manager, let characteristic = notifyCharacteristic else { return }
        while !pendingOutbound.isEmpty {
            guard manager.updateValue(pendingOutbound[0], for: characteristic, onSubscribedCentrals: nil) else {
                return // 队列已满，等 peripheralManagerIsReady 续传
            }
            pendingOutbound.removeFirst()
        }
    }

    private func installService() {
        guard let manager else { return }
        manager.removeAllServices()
        let writeCharacteristic = CBMutableCharacteristic(
            type: CBUUID(string: BuzzLinkConstants.cmdWriteUUIDString),
            properties: [.writeWithoutResponse],
            value: nil,
            permissions: [.writeable]
        )
        let notifyChar = CBMutableCharacteristic(
            type: CBUUID(string: BuzzLinkConstants.eventNotifyUUIDString),
            properties: [.notify],
            value: nil,
            permissions: []
        )
        let service = CBMutableService(
            type: CBUUID(string: BuzzLinkConstants.serviceUUIDString),
            primary: true
        )
        service.characteristics = [writeCharacteristic, notifyChar]
        notifyCharacteristic = notifyChar
        manager.add(service)
    }
}

// MARK: - CBPeripheralManagerDelegate（主队列回调，统一 hop 回 MainActor）

extension SimPeripheral: CBPeripheralManagerDelegate {

    nonisolated func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        let state = peripheral.state
        MainActor.assumeIsolated {
            logger.info("蓝牙状态变化：\(state.rawValue)")
            self.state = state
            if state == .poweredOn {
                installService()
            }
            onStateChange?(state)
        }
    }

    nonisolated func peripheralManager(_ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?) {
        if let error {
            MainActor.assumeIsolated {
                logger.error("添加 GATT 服务失败：\(error.localizedDescription)")
                onEvent?("添加 GATT 服务失败：\(error.localizedDescription)")
            }
        }
    }

    nonisolated func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
        if let error {
            MainActor.assumeIsolated {
                logger.error("广播失败：\(error.localizedDescription)")
                onEvent?("广播失败：\(error.localizedDescription)")
            }
        } else {
            MainActor.assumeIsolated {
                logger.info("广播已开始")
            }
        }
    }

    nonisolated func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]) {
        let values = requests.compactMap(\.value).filter { !$0.isEmpty }
        MainActor.assumeIsolated {
            for value in values {
                onWrite?(value)
            }
        }
    }

    nonisolated func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral, didSubscribeTo characteristic: CBCharacteristic) {
        MainActor.assumeIsolated {
            subscribedCentralCount += 1
            onSubscribed?()
        }
    }

    nonisolated func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral, didUnsubscribeFrom characteristic: CBCharacteristic) {
        MainActor.assumeIsolated {
            subscribedCentralCount = max(0, subscribedCentralCount - 1)
            onUnsubscribed?()
        }
    }

    nonisolated func peripheralManagerIsReady(toUpdateSubscribers peripheral: CBPeripheralManager) {
        MainActor.assumeIsolated {
            drainPending()
        }
    }
}
