import BuzzLinkCore
import SwiftUI

/// 协议控制台（T7 Phase B；视觉唯一依据 prototype v0.5 控制台场景）：
/// 工具行（4 预置 + 导出）→ 过滤行（方向/type 多选/文本/清除）→ 发送行
/// （hex 输入 + 发送；未连接/同步中禁用——拍板 A）→ 帧日志（Menlo 深色卡；
/// →/← 方向色；行点击展开完整 hex dump；DL_CHUNK ≥4 聚合折叠；自定义 hex 响应 3s 高亮）。
/// AX id 全量见 T7 计划 §7（data-ax 注释与实现逐一对齐）。
struct ConsoleView: View {
    @Bindable var viewModel: ConsoleViewModel
    let appModel: AppModel

    @State private var hexInput = ""
    @State private var hexError: String?
    @State private var hoveredRowID: Int?
    @State private var typePanelShown = false
    @FocusState private var hexFieldFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
            pageHeader
            ScrollView {
                VStack(alignment: .leading, spacing: DesignTokens.Space.s3) {
                    toolbarRow
                    filterRow
                    if typePanelShown {
                        typePanelInline
                    }
                    sendArea
                    logCard
                }
                .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
                .padding(.vertical, DesignTokens.Layout.mainPaddingVertical)
            }
        }
    }

    // MARK: - 页头

    private var pageHeader: some View {
        VStack(alignment: .leading, spacing: DesignTokens.Space.s1) {
            Text("协议控制台")
                .font(DesignTokens.FontToken.pageTitle)
            Text("开发者模式 · BuzzLink 帧日志与原始命令（可导出为固件测试向量）")
                .font(DesignTokens.FontToken.body)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, DesignTokens.Layout.mainPaddingHorizontal)
        .padding(.vertical, DesignTokens.Layout.mainPaddingVertical)
    }

    // MARK: - 工具行（4 预置 + Spacer + 导出）

    private var toolbarRow: some View {
        HStack(spacing: DesignTokens.Space.s2) {
            ForEach(ConsoleViewModel.Preset.allCases, id: \.rawValue) { preset in
                Button(preset.title) {
                    viewModel.send(preset)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .disabled(viewModel.sendGate != nil)
                .accessibilityIdentifier("console.presetButton.\(preset.rawValue)")
            }
            Spacer()
            Button("导出向量") {
                viewModel.export()
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
            .accessibilityIdentifier("console.exportButton")
        }
    }

    // MARK: - 过滤行（方向单选 + type 多选 + 文本 + 清除）

    private var filterRow: some View {
        HStack(spacing: DesignTokens.Space.s2) {
            Picker("方向", selection: directionBinding) {
                Text("全部").tag(FrameLogBuffer.Direction?.none)
                    .accessibilityIdentifier("console.directionFilter.all")
                Text("→ 上行").tag(FrameLogBuffer.Direction?.some(.tx))
                    .accessibilityIdentifier("console.directionFilter.tx")
                Text("← 下行").tag(FrameLogBuffer.Direction?.some(.rx))
                    .accessibilityIdentifier("console.directionFilter.rx")
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .frame(width: 200)
            .accessibilityIdentifier("console.directionFilter")

            // type 多选：内联展开面板——菜单弹层的 Toggle AX id 不透传（本机实测）、
            // popover 内容对 AX 遍历不可见（本机实测仅 1 窗口），内联内容在窗口
            // 主视图树 id 稳定可达（同 segmented 项方案；CI run 39098188609 label 查询教训）。
            Button(viewModel.hasTypeFilter ? "类型（\(viewModel.typeFilter.count)）▾" : "类型 ▾") {
                typePanelShown.toggle()
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
            .accessibilityIdentifier("console.typeFilter")

            TextField("文本过滤（payload/注释）", text: textBinding)
                .textFieldStyle(.roundedBorder)
                .controlSize(.small)
                .frame(maxWidth: 220)
                .font(DesignTokens.FontToken.caption)
                .accessibilityIdentifier("console.filterField")

            Button("清除") {
                viewModel.clear()
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
            .accessibilityIdentifier("console.clearButton")
            Spacer()
        }
    }

    private var directionBinding: Binding<FrameLogBuffer.Direction?> {
        Binding(
            get: { viewModel.directionFilter },
            set: { viewModel.setDirectionFilter($0) }
        )
    }

    /// type 多选内联面板（17 项 Toggle 两列网格，勾选状态即 AX value）
    private var typePanelInline: some View {
        VStack(alignment: .leading, spacing: DesignTokens.Space.s2) {
            HStack {
                Text("类型过滤（勾选取并集，拍板 A）")
                    .font(DesignTokens.FontToken.caption)
                    .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                Spacer()
                Button("完成") {
                    typePanelShown = false
                }
                .buttonStyle(.bordered)
                .controlSize(.mini)
                .accessibilityIdentifier("console.typeFilter.done")
            }
            LazyVGrid(columns: [GridItem(.fixed(200)), GridItem(.fixed(200))], alignment: .leading, spacing: 2) {
                ForEach(ConsoleViewModel.allTypes, id: \.self) { type in
                    Toggle(ConsoleViewModel.typeName(type), isOn: typePanelBinding(type))
                        .toggleStyle(.checkbox)
                        .controlSize(.small)
                        .accessibilityIdentifier("console.typeFilter.item.\(ConsoleViewModel.typeName(type))")
                }
            }
        }
        .padding(DesignTokens.Space.s3)
        .background(DesignTokens.ColorToken.surfaceCard2, in: RoundedRectangle(cornerRadius: 8))
        .accessibilityIdentifier("console.typeFilter.panel")
    }

    private func typePanelBinding(_ type: UInt8) -> Binding<Bool> {
        Binding(
            get: { viewModel.typeFilter.contains(type) },
            set: { _ in viewModel.toggleTypeFilter(type) }
        )
    }

    private var textBinding: Binding<String> {
        Binding(
            get: { viewModel.textFilter },
            set: { viewModel.setTextFilter($0) }
        )
    }

    // MARK: - 发送行（门控提示 + hex 输入 + 发送）

    private var sendArea: some View {
        VStack(alignment: .leading, spacing: DesignTokens.Space.s1) {
            if let gate = viewModel.sendGate {
                Text(gate == .syncing
                     ? "同步进行中，完成后可发送（设备页同步结束后自动恢复）"
                     : "连接设备后可发送命令（设备页连接 Beecord Watch）")
                    .font(DesignTokens.FontToken.caption)
                    .foregroundStyle(DesignTokens.ColorToken.stateWarning)
                    .accessibilityIdentifier("console.sendGateTip")
            }
            if let hexError {
                Text(hexError)
                    .font(DesignTokens.FontToken.caption)
                    .foregroundStyle(DesignTokens.ColorToken.stateDanger)
            }
            HStack(spacing: DesignTokens.Space.s2) {
                TextField("自定义 hex 整帧，如：beec 01 10 21 00 00 0001 0002 0000 c8f2",
                          text: $hexInput)
                    .textFieldStyle(.roundedBorder)
                    .controlSize(.small)
                    .font(.system(.caption, design: .monospaced))
                    .disabled(viewModel.sendGate != nil)
                    .focused($hexFieldFocused)
                    .onSubmit(sendHex)
                    .accessibilityIdentifier("console.hexField")
                Button("发送") {
                    sendHex()
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.small)
                .disabled(viewModel.sendGate != nil || hexInput.trimmingCharacters(in: .whitespaces).isEmpty)
                .accessibilityIdentifier("console.sendButton")
            }
        }
    }

    private func sendHex() {
        let result = viewModel.sendCustomHex(hexInput)
        hexError = result
        if result == nil {
            hexInput = ""
            hexFieldFocused = false
        }
    }

    // MARK: - 帧日志（深色卡）

    private var logCard: some View {
        Group {
            if viewModel.isLogEmpty {
                VStack(spacing: DesignTokens.Space.s3) {
                    Spacer().frame(height: DesignTokens.Space.s6)
                    Text("暂无帧记录")
                        .font(DesignTokens.FontToken.groupTitle)
                    Text("连接设备后发送预置命令或自定义 hex。")
                        .font(DesignTokens.FontToken.callout)
                        .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                    Spacer().frame(height: DesignTokens.Space.s6)
                }
                .frame(maxWidth: .infinity)
                .accessibilityIdentifier("console.emptyState")
            } else {
                logList
            }
        }
        .background(Color(red: 0.118, green: 0.118, blue: 0.141), in: RoundedRectangle(cornerRadius: 8))
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .strokeBorder(DesignTokens.ColorToken.surfaceCard2, lineWidth: 1)
        )
        // 注意：logTable 的 id 挂在 else 分支（logList）上而非本 Group——
        // Group 级 id 会把整组合成单个 AX 元素吞掉子 id（空态时 console.emptyState
        // 不可达，CI run 39096993559 实录）。
    }

    private var logList: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    ForEach(viewModel.displayRows) { row in
                        switch row.kind {
                        case .frame(let entry):
                            frameRow(entry)
                        case .aggregate(let firstRowID, let count, let seqRange):
                            aggregateRow(firstRowID: firstRowID, count: count, seqRange: seqRange)
                        }
                    }
                }
                .padding(DesignTokens.Space.s2)
            }
            .frame(minHeight: 320, idealHeight: 420)
            .accessibilityIdentifier("console.logTable")
            .onChange(of: viewModel.buffer.count) { _, _ in
                // 新帧滚动到底（P0 简化：总是滚；用户回看历史被打断留 P1 精细化）
                if let lastID = viewModel.displayRows.last?.id {
                    proxy.scrollTo(lastID, anchor: .bottom)
                }
            }
        }
    }

    private func frameRow(_ entry: FrameLogBuffer.Entry) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Button {
                viewModel.toggleExpanded(entry.rowID)
            } label: {
                HStack(spacing: DesignTokens.Space.s2) {
                    Text(String(format: "%08.3f", entry.ts / 1000))
                        .foregroundStyle(Color(red: 0.388, green: 0.388, blue: 0.4))
                    Text(entry.direction == .tx ? "→" : "←")
                        .foregroundStyle(entry.direction == .tx
                                         ? DesignTokens.ColorToken.stateSync
                                         : DesignTokens.ColorToken.stateSuccess)
                        .frame(width: 14)
                    Text(ConsoleViewModel.typeName(entry.frame.type))
                        .foregroundStyle(Color(red: 0.898, green: 0.753, blue: 0.482))
                    + Text(" 0x" + String(entry.frame.type, radix: 16, uppercase: true))
                        .foregroundStyle(Color(red: 0.388, green: 0.388, blue: 0.4))
                    Text(payloadSummary(entry.frame.payload))
                        .foregroundStyle(Color(red: 0.616, green: 0.616, blue: 0.651))
                        .lineLimit(1)
                        .truncationMode(.tail)
                    if let comment = entry.comment {
                        Text("# " + comment)
                            .foregroundStyle(Color(red: 0.388, green: 0.388, blue: 0.4))
                            .italic()
                            .lineLimit(1)
                    }
                    Spacer(minLength: 0)
                }
                .font(.system(.caption, design: .monospaced))
                .padding(.vertical, 2)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .background(hoveredRowID == entry.rowID
                        ? Color.white.opacity(0.06)
                        : (viewModel.highlightedRowID == entry.rowID
                           ? DesignTokens.ColorToken.stateSync.opacity(0.28)
                           : Color.clear))
            .onHover { hovering in
                hoveredRowID = hovering ? entry.rowID : (hoveredRowID == entry.rowID ? nil : hoveredRowID)
            }
            .accessibilityIdentifier("console.logTable.row.\(entry.rowID)")

            if viewModel.expandedIDs.contains(entry.rowID) {
                Text(viewModel.hexDump(for: entry))
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(Color(red: 0.616, green: 0.616, blue: 0.651))
                    .padding(DesignTokens.Space.s2)
                    .background(Color(red: 0.071, green: 0.071, blue: 0.086), in: RoundedRectangle(cornerRadius: 6))
                    .padding(.leading, DesignTokens.Space.s5)
                    .padding(.bottom, DesignTokens.Space.s1)
                    .accessibilityIdentifier("console.logTable.row.\(entry.rowID).hex")
            }
        }
    }

    private func aggregateRow(firstRowID: Int, count: Int, seqRange: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Button {
                viewModel.toggleExpanded(-(firstRowID + 1))   // 聚合展开键：负数编码
            } label: {
                HStack(spacing: DesignTokens.Space.s2) {
                    Text(String(format: "%08.3f", 0))
                        .foregroundStyle(Color(red: 0.388, green: 0.388, blue: 0.4))
                    Text("←")
                        .foregroundStyle(DesignTokens.ColorToken.stateSuccess)
                        .frame(width: 14)
                    Text("DL_CHUNK ×\(count)（已折叠，点击看明细）")
                        .foregroundStyle(Color(red: 0.898, green: 0.753, blue: 0.482))
                    Text(seqRange)
                        .foregroundStyle(Color(red: 0.616, green: 0.616, blue: 0.651))
                    Spacer(minLength: 0)
                }
                .font(.system(.caption, design: .monospaced))
                .padding(.vertical, 2)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("console.logTable.agg.\(firstRowID)")

            if viewModel.expandedIDs.contains(-(firstRowID + 1)) {
                // 聚合明细：复用过滤前的缓冲段（按首行 ID 定位展示）
                aggregateDetail(firstRowID: firstRowID)
            }
        }
    }

    /// 聚合展开明细：从缓冲按首行 ID 取**连续 CHUNK 段**（rowID 连续且同向）渲染；
    /// 防御性上限 200 行——防 12k 块一次性渲染（P1 可做分页/虚拟化）。
    private func aggregateDetail(firstRowID: Int) -> some View {
        let entries = viewModel.buffer.entries
        var segment: [FrameLogBuffer.Entry] = []
        if let start = entries.firstIndex(where: { $0.rowID == firstRowID }) {
            var previousRowID = firstRowID - 1
            for entry in entries[start...] {
                guard entry.frame.type == BuzzLinkFrameType.dlChunk.rawValue,
                      entry.direction == .rx,
                      entry.rowID == previousRowID + 1 else { break }
                segment.append(entry)
                previousRowID = entry.rowID
            }
        }
        return VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(segment.prefix(200)), id: \.rowID) { entry in
                frameRow(entry)
            }
            if segment.count > 200 {
                Text("… 共 \(segment.count) 块，仅渲染前 200 块明细")
                    .font(DesignTokens.FontToken.caption)
                    .foregroundStyle(DesignTokens.ColorToken.textTertiary)
                    .padding(.vertical, 2)
            }
        }
        .padding(.leading, DesignTokens.Space.s5)
    }



    /// payload hex 摘要（截断与原型一致 ≤96 字符）
    private func payloadSummary(_ payload: Data) -> String {
        guard !payload.isEmpty else { return "（空）" }
        let hex = payload.map { String(format: "%02x", $0) }.joined(separator: " ")
        return String(hex.prefix(96))
    }
}

#Preview("控制台") {
    ConsoleView(
        viewModel: ConsoleViewModel(appModel: AppModel(), deviceViewModel: previewDeviceVM()),
        appModel: AppModel()
    )
}

@MainActor
private func previewDeviceVM() -> DeviceViewModel {
    DeviceViewModel(appModel: AppModel(), settings: SettingsModel())
}
