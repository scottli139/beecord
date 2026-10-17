import AppKit
import BuzzLinkSync
import SwiftUI

/// 录音库（T6 Phase B）：左列表 + 右详情占位（播放器/波形/Tab 为 Phase C，接入点见详情区注释）。
/// 视觉依据 prototype v0.3；组件规格 DESIGN_SYSTEM §2（库列表行/确认对话框行）。
struct LibraryView: View {
    @Bindable var viewModel: LibraryViewModel
    let appModel: AppModel

    /// 删除确认目标（自绘 NSAlert 语义对话框；AX `common.confirmDialog`）
    @State private var deleteTarget: LibraryViewModel.Item?

    var body: some View {
        VStack(spacing: 0) {
            pageHeader
            Group {
                if viewModel.isEmpty && !viewModel.isScanning {
                    libraryEmptyState
                } else {
                    libraryContent
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .onAppear { viewModel.refresh() }
        .overlay {
            if let target = deleteTarget {
                deleteDialog(target)
            }
        }
    }

    // MARK: - 页头（§2.1 骨架：标题 + 副标题，左对齐）

    private var pageHeader: some View {
        VStack(alignment: .leading, spacing: DesignTokens.Space.s1) {
            Text("录音库")
                .font(DesignTokens.FontToken.pageTitle)
            Text("已同步录音的播放、转写与 AI 纪要（全部本地保存）")
                .font(DesignTokens.FontToken.body)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
        .padding(.vertical, DesignTokens.Layout.mainPaddingVertical)
    }

    // MARK: - 空态（原型 library-empty：卡片 + 聚簇 +【前往设备同步】）

    private var libraryEmptyState: some View {
        VStack(spacing: 0) {
            VStack(spacing: DesignTokens.Space.s3) {
                EmptyStateView(
                    systemImage: "waveform",
                    title: "录音库还是空的",
                    message: "连接 Beecord 设备并同步录音后，可在这里播放、转写和生成 AI 纪要。录音按录制时间新→旧排列。",
                    accessibilityID: "library.emptyState",
                    contentPadding: 0
                )
                Button("前往设备同步") {
                    appModel.selectPage(.device)
                }
                .buttonStyle(.borderedProminent)
                .accessibilityIdentifier("library.goToDeviceButton")
            }
            .padding(DesignTokens.Space.s8)
            .frame(maxWidth: .infinity, minHeight: DesignTokens.Layout.emptyCardMinHeight)
            .cardStyle()
        }
        .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
    }

    // MARK: - 两栏内容

    private var libraryContent: some View {
        HStack(alignment: .top, spacing: DesignTokens.Space.s4) {
            recordingList
            Divider()
            detailArea
        }
        .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
    }

    /// 左栏：排序标注 + 列表（列宽 280，规格见 DESIGN_SYSTEM §2 库列表行）
    private var recordingList: some View {
        VStack(alignment: .leading, spacing: DesignTokens.Space.s1) {
            HStack {
                Text("录音 \(viewModel.sortedItems.count) 条")
                Spacer()
                Text("按录制时间 新→旧")
            }
            .font(DesignTokens.FontToken.caption)
            .foregroundStyle(DesignTokens.ColorToken.textTertiary)
            .padding(.horizontal, DesignTokens.Space.s2)

            ScrollView {
                LazyVStack(spacing: DesignTokens.Space.s2) {
                    ForEach(viewModel.sortedItems) { item in
                        LibraryRowView(
                            item: item,
                            isSelected: viewModel.selectedID == item.id,
                            onSelect: { viewModel.select(item.id) }
                        )
                    }
                }
            }
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier("library.recordingTable")
        }
        .frame(width: 280)
    }

    /// 右栏：未选择态 / 详情占位
    @ViewBuilder
    private var detailArea: some View {
        if viewModel.selectedID == nil {
            VStack(spacing: 0) {
                VStack(spacing: DesignTokens.Space.s3) {
                    EmptyStateView(
                        systemImage: "list.bullet",
                        title: "未选择录音",
                        message: "从左侧选择一条录音，查看播放器、转写和 AI 纪要。",
                        accessibilityID: "library.noSelection",
                        contentPadding: 0
                    )
                    Button("选择最近录音") {
                        viewModel.selectRecent()
                    }
                    .buttonStyle(.borderedProminent)
                    .accessibilityIdentifier("library.selectRecentButton")
                }
                .padding(DesignTokens.Space.s8)
                .frame(maxWidth: .infinity, minHeight: DesignTokens.Layout.emptyCardMinHeight)
                .cardStyle()
            }
        } else if let item = viewModel.selectedItem {
            detailCard(item)
        }
    }

    // MARK: - 详情占位（Phase C 接入点：播放器/波形/Tab 替换 playerPlaceholder 区域）

    private func detailCard(_ item: LibraryViewModel.Item) -> some View {
        VStack(alignment: .leading, spacing: DesignTokens.Space.s3) {
            RenameField(
                initial: item.meta.title,
                onCommit: { viewModel.rename(id: item.id, title: $0) },
                onCancel: {}
            )
            metaRows(item)

            // Phase C 接入点：转码 ready 时在此渲染 PlayerBarView + WaveformView + library.detailTabs
            playerPlaceholder(item)

            HStack(spacing: DesignTokens.Space.s2) {
                Button("导出…") {}
                    .disabled(true)
                    .help("随播放器（T6 Phase C）交付")
                Button("显示于 Finder") {
                    viewModel.revealInFinder(id: item.id)
                }
                .accessibilityIdentifier("library.revealInFinderButton")
                Spacer()
                Button("删除") {
                    deleteTarget = item
                }
                .foregroundStyle(DesignTokens.ColorToken.stateDanger)
                .accessibilityIdentifier("library.deleteButton")
            }
        }
        .padding(DesignTokens.Space.s4)
        .cardStyle()
        .frame(maxWidth: .infinity, alignment: .topLeading)
    }

    private func metaRows(_ item: LibraryViewModel.Item) -> some View {
        let meta = item.meta
        return VStack(alignment: .leading, spacing: DesignTokens.Space.s1) {
            LabeledContent("来源", value: meta.source.deviceName.isEmpty ? "—" : meta.source.deviceName)
            LabeledContent("时长", value: FileTableView.durationText(meta.durationMs))
            LabeledContent("大小", value: ByteCountFormatter.string(fromByteCount: Int64(meta.sizeBytes), countStyle: .binary))
            if let recordedAt = meta.recordedAt {
                LabeledContent("录制时间", value: Self.dateText(recordedAt))
            }
            LabeledContent("格式", value: "\(meta.format) · \(meta.sampleRate / 1000) kHz")
        }
        .font(DesignTokens.FontToken.callout)
        .foregroundStyle(DesignTokens.ColorToken.textSecondary)
    }

    /// 转码未完成占位（原型 P0-2 联动；ready 后由 Phase C 播放器替换）
    @ViewBuilder
    private func playerPlaceholder(_ item: LibraryViewModel.Item) -> some View {
        switch item.transcode {
        case .ready:
            VStack(spacing: DesignTokens.Space.s2) {
                Image(systemName: "play.circle")
                    .font(.system(size: 28))
                Text("转码完成，播放器随 Phase C 交付")
                    .font(DesignTokens.FontToken.callout)
            }
            .foregroundStyle(DesignTokens.ColorToken.textTertiary)
            .frame(maxWidth: .infinity)
            .padding(.vertical, DesignTokens.Space.s8)
        case .running, .pending:
            HStack(spacing: DesignTokens.Space.s2) {
                ProgressView().controlSize(.small)
                Text(item.transcode == .running ? "转码中，稍后即可播放…" : "排队等待转码…")
            }
            .font(DesignTokens.FontToken.callout)
            .foregroundStyle(DesignTokens.ColorToken.textSecondary)
            .frame(maxWidth: .infinity)
            .padding(.vertical, DesignTokens.Space.s8)
        case .failed(let message):
            HStack(spacing: DesignTokens.Space.s2) {
                Image(systemName: "exclamationmark.circle.fill")
                Text("转码失败：\(message)")
                Button("重试") {
                    viewModel.retryTranscode(id: item.id)
                }
                .accessibilityIdentifier("library.retryTranscodeButton")
            }
            .font(DesignTokens.FontToken.callout)
            .foregroundStyle(DesignTokens.ColorToken.stateDanger)
            .frame(maxWidth: .infinity)
            .padding(.vertical, DesignTokens.Space.s8)
        }
    }

    // MARK: - 删除确认（自绘 NSAlert 语义：取消默认聚焦）

    private func deleteDialog(_ item: LibraryViewModel.Item) -> some View {
        ZStack {
            DesignTokens.ColorToken.scrim
                .ignoresSafeArea()
                .onTapGesture { deleteTarget = nil }
            VStack(alignment: .leading, spacing: DesignTokens.Space.s3) {
                Text("删除该录音？")
                    .font(DesignTokens.FontToken.groupTitle)
                Text("「\(item.meta.title)」将从录音库移除，未导出的原件将无法恢复。")
                    .font(DesignTokens.FontToken.callout)
                    .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                HStack(spacing: DesignTokens.Space.s2) {
                    Spacer()
                    Button("删除") {
                        deleteTarget = nil
                        viewModel.deleteRecording(id: item.id)
                    }
                    .foregroundStyle(DesignTokens.ColorToken.stateDanger)
                    .buttonStyle(.bordered)
                    .accessibilityIdentifier("library.deleteConfirmButton")
                    Button("取消") {
                        deleteTarget = nil
                    }
                    .buttonStyle(.borderedProminent)
                    .accessibilityIdentifier("library.deleteCancelButton")
                }
            }
            .padding(DesignTokens.Space.s5)
            .frame(width: 380)
            .cardStyle()
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier("common.confirmDialog")
        }
    }

    private static func dateText(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd HH:mm"
        return formatter.string(from: date)
    }
}

/// 列表行（DESIGN_SYSTEM §2 库列表行；自绘 hover 态按 §2.2）。
private struct LibraryRowView: View {
    let item: LibraryViewModel.Item
    let isSelected: Bool
    let onSelect: () -> Void

    @State private var isHovered = false

    var body: some View {
        Button(action: onSelect) {
            HStack(alignment: .top, spacing: DesignTokens.Space.s2) {
                VStack(alignment: .leading, spacing: DesignTokens.Space.s1) {
                    Text(item.meta.title)
                        .font(DesignTokens.FontToken.groupTitle)
                        .foregroundStyle(DesignTokens.ColorToken.textPrimary)
                        .lineLimit(1)
                        .truncationMode(.tail)
                        .accessibilityIdentifier("library.recordingTable.row.\(item.id).title")
                    Text(metaLine)
                        .font(DesignTokens.FontToken.caption)
                        .foregroundStyle(DesignTokens.ColorToken.textTertiary)
                }
                Spacer(minLength: 0)
                VStack(alignment: .trailing, spacing: DesignTokens.Space.s1) {
                    Text(transcodeBadgeText)
                        .font(.system(size: 10))
                        .padding(.horizontal, DesignTokens.Space.s1 + 2)
                        .padding(.vertical, 1)
                        .background(transcodeBadgeColor.opacity(0.14), in: RoundedRectangle(cornerRadius: 4))
                        .foregroundStyle(transcodeBadgeColor)
                        .accessibilityIdentifier("library.recordingTable.row.\(item.id).transcodeBadge")
                    Text("未转写")
                        .font(.system(size: 10))
                        .padding(.horizontal, DesignTokens.Space.s1 + 2)
                        .padding(.vertical, 1)
                        .background(Color(.secondaryLabelColor).opacity(0.12), in: RoundedRectangle(cornerRadius: 4))
                        .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                        .accessibilityIdentifier("library.recordingTable.row.\(item.id).transcribeBadge")
                }
            }
            .padding(.horizontal, DesignTokens.Space.s3)
            .padding(.vertical, DesignTokens.Space.s3 - 2)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .background(DesignTokens.ColorToken.surfaceCard, in: RoundedRectangle(cornerRadius: 8))
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .strokeBorder(
                    isSelected ? Color.accentColor : (isHovered ? Color.accentColor.opacity(0.4) : DesignTokens.ColorToken.surfaceCard2),
                    lineWidth: 1
                )
        )
        .shadow(
            color: isSelected ? Color.accentColor.opacity(0.3) : .clear,
            radius: 1
        )
        .onHover { isHovered = $0 }
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("library.recordingTable.row.\(item.id)")
    }

    private var metaLine: String {
        let date = item.meta.recordedAt.map { d -> String in
            let f = DateFormatter()
            f.locale = Locale(identifier: "en_US_POSIX")
            f.dateFormat = "MM-dd HH:mm"
            return f.string(from: d)
        } ?? "未知时间"
        return "\(date) · \(FileTableView.durationText(item.meta.durationMs))"
    }

    private var transcodeBadgeText: String {
        switch item.transcode {
        case .ready: return "已就绪"
        case .running: return "转码中"
        case .pending: return "待转码"
        case .failed: return "转码失败"
        }
    }

    private var transcodeBadgeColor: Color {
        switch item.transcode {
        case .ready: return DesignTokens.ColorToken.stateSuccess
        case .running: return Color.accentColor
        case .pending: return DesignTokens.ColorToken.textTertiary
        case .failed: return DesignTokens.ColorToken.stateDanger
        }
    }
}

#Preview("空态") {
    LibraryView(
        viewModel: LibraryViewModel(appModel: AppModel(), settings: SettingsModel()),
        appModel: AppModel()
    )
}

#Preview("列表 · 深色") {
    let appModel = AppModel()
    let vm = LibraryViewModel(appModel: appModel, settings: SettingsModel())
    return LibraryView(viewModel: vm, appModel: appModel)
        .preferredColorScheme(.dark)
}
