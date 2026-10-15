import SwiftUI

/// 协议日志区（环形缓冲，最新在前；方向符号 → 上行 / ← 下行 / · 系统）。
struct SimLogSection: View {
    @Bindable var model: SimDeviceModel

    var body: some View {
        Section {
            List(model.logEntries.reversed()) { entry in
                HStack(spacing: 8) {
                    Text(entry.time, format: .dateTime.hour().minute().second())
                        .foregroundStyle(.tertiary)
                    Text(entry.direction.rawValue)
                        .foregroundStyle(directionColor(entry.direction))
                    Text(entry.text)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.tail)
                }
                .font(.system(.callout, design: .monospaced))
            }
            .frame(minHeight: 160, maxHeight: 240)
            .accessibilityIdentifier("sim.log.list")
        } header: {
            HStack {
                Text("日志")
                Spacer()
                Button("清空") { model.clearLog() }
                    .accessibilityIdentifier("sim.log.clearButton")
            }
        }
    }

    private func directionColor(_ direction: SimLogEntry.Direction) -> Color {
        switch direction {
        case .inbound: .blue      // state.sync
        case .outbound: .green    // state.success
        case .system: .secondary
        }
    }
}

#Preview {
    Form {
        SimLogSection(model: SimDeviceModel())
    }
    .formStyle(.grouped)
}
