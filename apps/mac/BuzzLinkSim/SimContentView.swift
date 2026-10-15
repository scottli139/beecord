import BuzzLinkCore
import BuzzLinkSimCore
import SwiftUI

/// BuzzLinkSim 主界面：状态 / 设备档案 / 文件库 / 故障注入 / 日志。
/// AX 规则遵守 DESIGN_SYSTEM §4（前缀 `sim.`）。
struct SimContentView: View {
    @Bindable var model: SimDeviceModel

    var body: some View {
        Form {
            statusSection
            profileSection
            filesSection
            SimFaultsSection(model: model)
            SimLogSection(model: model)
        }
        .formStyle(.grouped)
        #if os(macOS)
        .frame(minWidth: 560, minHeight: 660)
        #endif
    }

    // MARK: - 状态

    private var statusSection: some View {
        Section("状态") {
            LabeledContent("蓝牙", value: model.bluetoothStateText)
                .accessibilityIdentifier("sim.status.bluetooth")
            Toggle("开始广播", isOn: Binding(
                get: { model.advertisingWanted },
                set: { model.setAdvertisingWanted($0) }
            ))
            .accessibilityIdentifier("sim.advertise.toggle")
            Toggle("Loopback 同机通道（127.0.0.1:\(BuzzLinkConstants.loopbackPort)）", isOn: Binding(
                get: { model.loopbackWanted },
                set: { model.setLoopbackWanted($0) }
            ))
            .accessibilityIdentifier("sim.loopback.toggle")
            LabeledContent("Loopback 客户端", value: "\(model.loopbackClientCount)")
                .accessibilityIdentifier("sim.status.loopback")
            LabeledContent("设备名", value: model.engine.profile.deviceName)
                .accessibilityIdentifier("sim.status.deviceName")
            LabeledContent("已连接 Central", value: "\(model.subscribedCentralCount)")
                .accessibilityIdentifier("sim.status.centrals")
            if let progress = model.streamProgress {
                LabeledContent("推流", value: progress)
                    .font(.callout)
                    .accessibilityIdentifier("sim.status.stream")
            }
            if model.isSilencedNow {
                Text("静默中（模拟断连，不响应任何命令）")
                    .font(.callout)
                    .foregroundStyle(.red)
                    .accessibilityIdentifier("sim.status.silenceBanner")
            }
        }
    }

    // MARK: - 设备档案

    private var profileSection: some View {
        Section("设备档案") {
            LabeledContent("型号", value: model.engine.profile.model)
            LabeledContent("固件版本", value: model.engine.profile.fwVersion)
            Stepper(
                "电量 \(model.engine.profile.battery)%",
                value: $model.engine.profile.battery,
                in: 0...100
            )
            .accessibilityIdentifier("sim.profile.battery.stepper")
            Toggle("充电中", isOn: $model.engine.profile.isCharging)
                .accessibilityIdentifier("sim.profile.charging.toggle")
            LabeledContent(
                "存储",
                value: "\(Self.sizeText(Int64(model.engine.profile.storageFree))) 可用 / 共 \(Self.sizeText(Int64(model.engine.profile.storageTotal)))"
            )
            .accessibilityIdentifier("sim.profile.storage")
        }
    }

    // MARK: - 文件库

    private var filesSection: some View {
        Section("文件库（\(model.engine.visibleFiles.count)）") {
            ForEach(model.engine.visibleFiles) { file in
                LabeledContent {
                    Text("\(Self.durationText(file.durationMs)) · \(Self.sizeText(Int64(file.data.count)))")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                } label: {
                    Text(file.id)
                        .font(.callout)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                .accessibilityIdentifier("sim.files.row.\(file.id)")
            }
        }
    }

    // MARK: - 格式化

    static func sizeText(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .binary)
    }

    static func durationText(_ ms: UInt32) -> String {
        let totalSeconds = Int(ms) / 1_000
        return String(format: "%d:%02d", totalSeconds / 60, totalSeconds % 60)
    }
}

#Preview {
    SimContentView(model: SimDeviceModel())
}
