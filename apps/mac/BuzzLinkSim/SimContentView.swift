import BuzzLinkCore
import SwiftUI

/// 占位首页（T3 起实现 BuzzLink 虚拟设备：BLE 外设 + 故障注入，见 TESTING.md §5）。
struct SimContentView: View {
    var body: some View {
        VStack(spacing: 12) {
            Text("BuzzLinkSim")
                .font(.title)
            Text("BuzzLink protocol v\(BuzzLinkConstants.version)")
                .foregroundStyle(.secondary)
        }
        .frame(minWidth: 480, minHeight: 280)
        .accessibilityIdentifier("sim.placeholder")
    }
}

#Preview {
    SimContentView()
}
