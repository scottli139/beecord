import BuzzLinkSimCore
import SwiftUI

/// 故障注入区（TESTING.md §5 七条矩阵一一对应；默认全关 = 合法设备行为）。
struct SimFaultsSection: View {
    @Bindable var model: SimDeviceModel

    var body: some View {
        Section {
            Toggle("随机丢块（2%）", isOn: Binding(
                get: { model.engine.faults.dropChunkProbability > 0 },
                set: { model.engine.faults.dropChunkProbability = $0 ? 0.02 : 0 }
            ))
            .accessibilityIdentifier("sim.faults.dropChunks.toggle")

            Toggle("限速 1 KB/s", isOn: Binding(
                get: { model.engine.faults.throttleBytesPerSecond != nil },
                set: { model.engine.faults.throttleBytesPerSecond = $0 ? 1_024 : nil }
            ))
            .accessibilityIdentifier("sim.faults.throttle.toggle")

            Toggle("推流中途断连（静默 \(Int(model.engine.faults.silentDuration))s）", isOn: $model.engine.faults.silentMidDownload)
                .accessibilityIdentifier("sim.faults.silence.toggle")

            Toggle("末帧 CRC 错误", isOn: $model.engine.faults.corruptFinalChunkCRC)
                .accessibilityIdentifier("sim.faults.corruptCRC.toggle")

            Toggle("空文件列表", isOn: $model.engine.faults.emptyFileList)
                .accessibilityIdentifier("sim.faults.emptyList.toggle")

            Toggle("异常文件名（超长 / 非法字符）", isOn: $model.engine.faults.hostileFileNames)
                .accessibilityIdentifier("sim.faults.hostileNames.toggle")

            Toggle("存储将满（>90%）", isOn: $model.engine.faults.storageNearlyFull)
                .accessibilityIdentifier("sim.faults.storageFull.toggle")
        } header: {
            Text("故障注入")
        } footer: {
            Text("默认全关即合法设备。真断连路径请直接退出本 App；故障项详见 TESTING.md §5。")
                .font(.caption)
        }
    }
}

#Preview {
    Form {
        SimFaultsSection(model: SimDeviceModel())
    }
    .formStyle(.grouped)
}
