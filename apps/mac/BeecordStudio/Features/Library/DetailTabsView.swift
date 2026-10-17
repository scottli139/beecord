import SwiftUI

/// 详情内 Tab（prototype v0.3 / P0-5 定稿）：原型为**下划线页签**（accent 2px 下划线 +
/// active 加重），非胶囊分段；自绘控件按 §2.2 补 hover（fill.hover）。
struct DetailTabsView: View {
    private enum Tab: String, CaseIterable, Identifiable {
        case transcript = "转写"
        case summary = "AI 纪要"
        var id: String { rawValue }
    }

    @State private var selection: Tab = .transcript
    @State private var hovered: Tab?

    var body: some View {
        VStack(spacing: DesignTokens.Space.s3) {
            HStack(spacing: DesignTokens.Space.s1) {
                ForEach(Tab.allCases) { tab in
                    tabButton(tab)
                }
                Spacer()
            }
            .overlay(alignment: .bottom) {
                Divider()  // 页签底线（原型 .tabs border-bottom）
            }

            switch selection {
            case .transcript:
                // P0-5 终稿：转写为 P1（T9），占位引导
                placeholder(
                    message: "尚未转写（本地 whisper，内容不出本机）",
                    buttonTitle: "开始转写",
                    tooltip: "P1 可用（T9 本地转写）"
                )
            case .summary:
                // P0-5 终稿：纪要为 P1（T10），占位引导
                placeholder(
                    message: "生成会议纪要需要已配置的 AI 端点与完成转写",
                    buttonTitle: "生成纪要",
                    tooltip: "P1 可用（T10 AI 纪要）"
                )
            }
        }
        .animation(.easeOut(duration: DesignTokens.Motion.micro), value: selection)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("library.detailTabs")
    }

    private func tabButton(_ tab: Tab) -> some View {
        let isActive = selection == tab
        return Button {
            selection = tab
        } label: {
            Text(tab.rawValue)
                .font(DesignTokens.FontToken.body)
                .fontWeight(isActive ? .semibold : .regular)
                .foregroundStyle(isActive ? Color.accentColor : DesignTokens.ColorToken.textSecondary)
                .padding(.horizontal, DesignTokens.Space.s3)   // 原型 padding 6×14 → 就近刻度 8×12
                .padding(.vertical, DesignTokens.Space.s2)
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
                .overlay(alignment: .bottom) {
                    if isActive {
                        Color.accentColor.frame(height: 2)  // 原型 border-bottom 2px accent
                    }
                }
        }
        .buttonStyle(.plain)
        .background {
            if hovered == tab {
                Color(.labelColor).opacity(0.05)  // §2.2 fill.hover（深浅自适应）
            }
        }
        .onHover { hovering in
            hovered = hovering ? tab : nil
        }
        .accessibilityAddTraits(isActive ? [.isSelected] : [])
    }

    private func placeholder(message: String, buttonTitle: String, tooltip: String) -> some View {
        VStack(spacing: DesignTokens.Space.s3) {
            Text(message)
                .font(DesignTokens.FontToken.body)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
            Button(buttonTitle) {}
                .buttonStyle(.borderedProminent)
                .disabled(true)
                .help(tooltip)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, DesignTokens.Space.s6)
    }
}
