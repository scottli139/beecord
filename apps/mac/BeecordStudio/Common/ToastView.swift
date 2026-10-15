import SwiftUI

/// Toast 消息（R8：identifier `common.toast`，文本可被轮询断言）。
struct ToastMessage: Equatable {
    enum Kind: Equatable {
        case info
        case error
    }

    let kind: Kind
    let text: String
}

/// Toast 组件（DESIGN_SYSTEM §2：底部居中胶囊，12px 字；2s，错误 3s）。
struct ToastView: View {
    let message: ToastMessage

    var body: some View {
        HStack(spacing: DesignTokens.Space.s2) {
            Image(systemName: message.kind == .error ? "exclamationmark.circle.fill" : "info.circle.fill")
            Text(message.text)
                .font(DesignTokens.FontToken.callout)
        }
        .foregroundStyle(.white)
        .padding(.horizontal, DesignTokens.Space.s4)
        .padding(.vertical, DesignTokens.Space.s2)
        .background(.black.opacity(0.8), in: Capsule())
        .shadow(color: DesignTokens.ColorToken.scrim, radius: DesignTokens.Space.s2, y: 1)
        .accessibilityIdentifier("common.toast")
    }
}

#Preview("信息") {
    ToastView(message: ToastMessage(kind: .info, text: "已保存设置"))
}

#Preview("错误 · 深色") {
    ToastView(message: ToastMessage(kind: .error, text: "下载失败，请重试"))
        .preferredColorScheme(.dark)
}
