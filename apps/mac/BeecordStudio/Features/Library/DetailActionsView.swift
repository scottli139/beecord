import AppKit
import BuzzLinkAudio
import SwiftUI

/// 详情操作行（prototype v0.3）：导出 m4a/wav/ogg（NSSavePanel）+ 显示于 Finder + 删除（红字）。
/// 全按钮 `.fixedSize()` 防截断；窗口最小宽度保证（见 DesignTokens.Layout.windowMinWidth）。
struct DetailActionsView: View {
    let itemID: String
    let directoryURL: URL
    let onDelete: () -> Void
    let onReveal: () -> Void
    let onExported: (Result<Void, Error>) -> Void

    var body: some View {
        HStack(spacing: DesignTokens.Space.s2) {
            Button("导出 m4a") { runExport(format: "m4a") }
                .fixedSize()
                .accessibilityIdentifier("library.exportButton.m4a")
            Button("导出 wav") { runExport(format: "wav") }
                .fixedSize()
                .accessibilityIdentifier("library.exportButton.wav")
            Button("导出 ogg（原始）") { runExport(format: "ogg") }
                .fixedSize()
                .accessibilityIdentifier("library.exportButton.ogg")
            Button("显示于 Finder", action: onReveal)
                .fixedSize()
                .accessibilityIdentifier("library.revealInFinderButton")
            Spacer()
            Button("删除", action: onDelete)
                .fixedSize()
                .foregroundStyle(DesignTokens.ColorToken.stateDanger)
                .accessibilityIdentifier("library.deleteButton")
        }
    }

    private func runExport(format: String) {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "\(itemID).\(format)"
        guard panel.runModal() == .OK, let destination = panel.url else { return }
        Task {
            let result: Result<Void, Error> = await {
                do {
                    try await AudioExporter.export(sourceDirectory: directoryURL, format: format, destination: destination)
                    return .success(())
                } catch {
                    return .failure(error)
                }
            }()
            onExported(result)
        }
    }
}
