import AppKit
import BuzzLinkAudio
import SwiftUI

/// 详情操作行（已定案 2026-10-17：方案 A"导出合并"，T6 计划范围表，原型方案A 场景评审通过）：
/// 【导出 ▾】下拉（导出 m4a / 导出 wav / 导出 ogg（原始））+ 显示于 Finder + Spacer + 删除（红字）。
/// 固有宽 ~244（对照原五按钮 ~460），详情列 ≥350 的所有支持窗宽单行成立——
/// 无窄宽断行兜底（断两行逻辑随本次定案整体移除）。
struct DetailActionsView: View {
    let itemID: String
    let directoryURL: URL
    let onDelete: () -> Void
    let onReveal: () -> Void
    let onExported: (Result<Void, Error>) -> Void

    var body: some View {
        HStack(spacing: DesignTokens.Space.s2) {
            Menu {
                Button("导出 m4a") { runExport(format: "m4a") }
                    .accessibilityIdentifier("library.exportMenuItem.m4a")
                Button("导出 wav") { runExport(format: "wav") }
                    .accessibilityIdentifier("library.exportMenuItem.wav")
                Button("导出 ogg（原始）") { runExport(format: "ogg") }
                    .accessibilityIdentifier("library.exportMenuItem.ogg")
            } label: {
                Text("导出")
            }
            .accessibilityIdentifier("library.exportButton")
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
