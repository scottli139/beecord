import SwiftUI

/// 内联重命名输入框：return 提交、Esc 取消；失焦提交（与 macOS 惯例一致）。
struct RenameField: View {
    let initial: String
    let onCommit: (String) -> Void
    let onCancel: () -> Void

    @State private var draft: String
    @FocusState private var focused: Bool

    init(initial: String, onCommit: @escaping (String) -> Void, onCancel: @escaping () -> Void) {
        self.initial = initial
        self.onCommit = onCommit
        self.onCancel = onCancel
        _draft = State(initialValue: initial)
    }

    var body: some View {
        TextField("", text: $draft)
            .textFieldStyle(.plain)
            .font(DesignTokens.FontToken.groupTitle)
            .focused($focused)
            .onAppear { focused = true }
            .onSubmit { commit() }
            .onKeyPress(.escape) {
                onCancel()
                return .handled
            }
            .onExitCommand { commit() }
            .accessibilityIdentifier("library.renameField")
            .accessibilityLabel("录音标题")
    }

    private func commit() {
        let trimmed = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty || trimmed == initial {
            onCancel()
        } else {
            onCommit(trimmed)
        }
    }
}
