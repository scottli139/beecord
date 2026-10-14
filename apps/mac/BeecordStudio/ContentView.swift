import BuzzLinkCore
import SwiftUI

/// 占位首页（T1 工程骨架验证用，T2 起替换为正式 UI）。
struct ContentView: View {
    var body: some View {
        VStack(spacing: 12) {
            Text("Beecord Studio")
                .font(.title)
            Text("BuzzLink protocol v\(BuzzLinkConstants.version)")
                .foregroundStyle(.secondary)
        }
        .frame(minWidth: 480, minHeight: 280)
        .accessibilityIdentifier("studio.placeholder")
    }
}

#Preview {
    ContentView()
}
