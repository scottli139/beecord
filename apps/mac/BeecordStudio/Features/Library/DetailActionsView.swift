import AppKit
import BuzzLinkAudio
import SwiftUI

/// 详情操作行（prototype v0.3）：导出 m4a/wav/ogg（NSSavePanel）+ 显示于 Finder + 删除（红字）。
/// 全按钮 `.fixedSize()` 防截断；窗口最小宽度保证（见 DesignTokens.Layout.windowMinWidth）。
/// 窄宽自动断两行（导出组一行、「显示于 Finder」+「删除」第二行）——否则行宽溢出窗口右缘，
/// 最右"删除"按钮 AX frame 落窗外不可点（CI run 39040395432 实录）。
/// 断行开关由 LibraryView 经 `twoRows` 传入（按页内容宽推导详情列宽，阈值 `singleRowMinWidth`）：
/// 不在本视图内自测宽度——溢出态下自测拿到的是被撑宽后的实际宽，会造成"宽版→撑宽→仍宽版"死锁。
struct DetailActionsView: View {
    let itemID: String
    let directoryURL: URL
    /// 窄宽模式：true = 操作行断两行。
    let twoRows: Bool
    let onDelete: () -> Void
    let onReveal: () -> Void
    let onExported: (Result<Void, Error>) -> Void

    /// 单行版固有宽（5 按钮 + 间距实测）；详情列低于此阈值必须断两行。
    static let singleRowMinWidth: CGFloat = 460

    var body: some View {
        if twoRows {
            twoRowsLayout
        } else {
            singleRow
        }
    }

    /// 宽版（原型定稿）：五个按钮一行，Spacer 右推。
    private var singleRow: some View {
        HStack(spacing: DesignTokens.Space.s2) {
            exportButtons
            Button("显示于 Finder", action: onReveal)
                .fixedSize()
                .accessibilityIdentifier("library.revealInFinderButton")
            Spacer()
            deleteButton
        }
    }

    /// 窄版：导出组一行，Finder + 删除第二行右对齐。
    private var twoRowsLayout: some View {
        VStack(alignment: .trailing, spacing: DesignTokens.Space.s2) {
            HStack(spacing: DesignTokens.Space.s2) {
                exportButtons
                Spacer(minLength: 0)
            }
            HStack(spacing: DesignTokens.Space.s2) {
                Button("显示于 Finder", action: onReveal)
                    .fixedSize()
                    .accessibilityIdentifier("library.revealInFinderButton")
                Spacer(minLength: 0)
                deleteButton
            }
        }
    }

    private var exportButtons: some View {
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
        }
    }

    private var deleteButton: some View {
        Button("删除", action: onDelete)
            .fixedSize()
            .foregroundStyle(DesignTokens.ColorToken.stateDanger)
            .accessibilityIdentifier("library.deleteButton")
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
