import BuzzLink
import SwiftUI

/// 设备信息卡（ux §3.1/§4：型号/固件/电量（<15% 橙色）/存储用量条/>90% 警告条/【断开】）。
struct DeviceInfoCard: View {
    let info: BuzzLinkDeviceInfo
    let isStorageNearlyFull: Bool
    let onDisconnect: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: DesignTokens.Space.s3) {
            HStack {
                Text(info.deviceName.isEmpty ? info.model : info.deviceName)
                    .font(DesignTokens.FontToken.groupTitle)
                Spacer()
                Button("断开") {
                    onDisconnect()
                }
                .accessibilityIdentifier("device.disconnectButton")
            }
            HStack(spacing: DesignTokens.Space.s6) {
                LabeledContent("型号", value: info.model)
                LabeledContent("固件", value: info.fwVersion)
            }
            LabeledContent {
                HStack(spacing: DesignTokens.Space.s2) {
                    Image(systemName: batteryIcon)
                    Text(batteryText)
                        .font(DesignTokens.FontToken.digit)
                }
                .foregroundStyle(isBatteryLow ? DesignTokens.ColorToken.stateWarning : DesignTokens.ColorToken.textPrimary)
            } label: {
                Text("电量")
            }
            .accessibilityIdentifier("device.infoCard.battery")
            VStack(alignment: .leading, spacing: DesignTokens.Space.s1) {
                HStack {
                    Text("存储")
                        .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                    Spacer()
                    Text("\(Self.sizeText(usedBytes)) / \(Self.sizeText(info.storageTotal))")
                        .font(DesignTokens.FontToken.digit)
                        .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                }
                ProgressView(value: storageFraction)
                    .progressViewStyle(.linear)
            }
            .accessibilityIdentifier("device.infoCard.storage")
            if isStorageNearlyFull {
                HStack(spacing: DesignTokens.Space.s2) {
                    Image(systemName: "exclamationmark.triangle.fill")
                    Text("设备存储即将占满（>90%），请在设备上清理录音后再同步")
                }
                .font(DesignTokens.FontToken.callout)
                .foregroundStyle(DesignTokens.ColorToken.stateWarning)
                .accessibilityIdentifier("device.storageWarning")
            }
        }
        .padding(DesignTokens.Space.s4)
        .background(DesignTokens.ColorToken.surfaceCard, in: RoundedRectangle(cornerRadius: DesignTokens.Radius.md))
        .overlay(
            RoundedRectangle(cornerRadius: DesignTokens.Radius.md)
                .strokeBorder(.separator, lineWidth: 1)
        )
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("device.deviceCard")
    }

    private var isBatteryLow: Bool { info.battery < 15 }

    private var batteryText: String {
        let percent = "\(info.battery)%"
        return info.isCharging ? "\(percent)（充电中）" : percent
    }

    private var batteryIcon: String {
        if info.isCharging { return "battery.100percent.bolt" }
        switch info.battery {
        case ..<15: return "battery.0percent"
        case ..<50: return "battery.25percent"
        case ..<80: return "battery.50percent"
        default: return "battery.75percent"
        }
    }

    private var usedBytes: UInt64 {
        info.storageTotal > info.storageFree ? info.storageTotal - info.storageFree : 0
    }

    private var storageFraction: Double {
        guard info.storageTotal > 0 else { return 0 }
        return min(1, Double(usedBytes) / Double(info.storageTotal))
    }

    static func sizeText(_ bytes: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .binary)
    }
}

#Preview("浅色") {
    DeviceInfoCard(
        info: BuzzLinkDeviceInfo(
            model: "BCD-DEMO",
            fwVersion: "0.9.1",
            protocolVersion: 1,
            deviceName: "Beecord-测试机",
            battery: 62,
            isCharging: false,
            storageTotal: 32 * 1024 * 1024 * 1024,
            storageFree: 4 * 1024 * 1024 * 1024
        ),
        isStorageNearlyFull: true,
        onDisconnect: {}
    )
    .padding()
}

#Preview("低电量 · 深色") {
    DeviceInfoCard(
        info: BuzzLinkDeviceInfo(
            model: "BCD-DEMO",
            fwVersion: "0.9.1",
            protocolVersion: 1,
            deviceName: "Beecord-测试机",
            battery: 8,
            isCharging: true,
            storageTotal: 32 * 1024 * 1024 * 1024,
            storageFree: 30 * 1024 * 1024 * 1024
        ),
        isStorageNearlyFull: false,
        onDisconnect: {}
    )
    .padding()
    .preferredColorScheme(.dark)
}
