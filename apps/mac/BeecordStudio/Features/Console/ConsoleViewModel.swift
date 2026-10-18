import AppKit
import BuzzLink
import BuzzLinkCore
import BuzzLinkSync
import Foundation
import os

/// 协议控制台视图模型（T7 Phase B）：订阅 FrameTapTransport 事件流驱动帧日志；
/// 预置命令/自定义 hex 发送（sendRaw）、过滤、行展开、导出、发送区门控
/// （未连接/同步中禁用——拍板 A）、自定义 hex 同 session RX 3s 高亮（拍板 A）。
///
/// 线程模型：@MainActor @Observable；buffer 为值语义 struct，append 后整体回写触发刷新。
/// 性能：派生展示行（过滤+聚合）为 O(n)（n ≤ 5000），每事件重算实测 <1ms；
/// 若 profiling 超标再引入 flush 节流（事件先入暂存队列、按 runloop 合帧）。
@MainActor
@Observable
final class ConsoleViewModel {

    /// 发送区禁用原因（nil = 可发送；拍板 A：同步中禁用；未连接禁用）
    enum SendGate: Equatable {
        case disconnected
        case syncing
    }

    /// 展示行（帧个体或 CHUNK 聚合段）
    struct DisplayRow: Identifiable, Equatable {
        enum Kind: Equatable {
            case frame(FrameLogBuffer.Entry)
            case aggregate(firstRowID: Int, count: Int, seqRange: String)
        }
        let kind: Kind
        /// SwiftUI 稳定 id：个体 "f-<rowID>"，聚合 "a-<rowID>"（首块行 ID）
        var id: String
    }

    /// 预置命令（BUZZLINK.md §5.1）
    enum Preset: String, CaseIterable {
        case list, info, begin, verify
        var title: String {
            switch self {
            case .list: return "列文件"
            case .info: return "查信息"
            case .begin: return "开始下载"
            case .verify: return "结束校验"
            }
        }
    }

    private(set) var buffer = FrameLogBuffer()
    private(set) var expandedIDs = Set<Int>()
    private(set) var directionFilter: FrameLogBuffer.Direction?
    private(set) var typeFilter = Set<UInt8>()
    private(set) var textFilter = ""
    /// 高亮行（自定义 hex 响应反馈，拍板 A：3s）
    private(set) var highlightedRowID: Int?

    let deviceViewModel: DeviceViewModel

    private let appModel: AppModel
    private let logger = Logger(subsystem: "com.beeuc.beecord.studio", category: "console")
    private let tapConsumption = ConsumptionHandle()
    private var highlightTask: Task<Void, Never>?
    /// 待高亮 session（自定义 hex 发送后期望的同 session RX；nil = 无）
    private var pendingHighlightSession: UInt8?

    init(appModel: AppModel, deviceViewModel: DeviceViewModel) {
        self.appModel = appModel
        self.deviceViewModel = deviceViewModel
        tapConsumption.add(Task { [weak self] in
            guard let self else { return }
            for await event in deviceViewModel.frameTap.events {
                await self.ingest(event)
            }
        })
    }

    deinit {
        tapConsumption.cancelAll()
    }

    // MARK: - 派生状态

    /// 发送区门控：同步中（拍板 A）> 未连接
    var sendGate: SendGate? {
        switch deviceViewModel.snapshot.state {
        case .syncing, .cancelling:
            return .syncing
        case .idle:
            return .disconnected
        default:
            return nil
        }
    }

    var isLogEmpty: Bool { buffer.isEmpty }

    var filter: FrameLogBuffer.Filter {
        FrameLogBuffer.Filter(direction: directionFilter, types: typeFilter, text: textFilter)
    }

    /// 展示行：过滤 → 连续 ≥4 块同向 DL_CHUNK 聚合折叠（计划 §6 风险对策的 UI 形态）
    var displayRows: [DisplayRow] {
        let entries = buffer.filtered(filter)
        var rows: [DisplayRow] = []
        var index = 0
        while index < entries.count {
            let entry = entries[index]
            if entry.frame.type == BuzzLinkFrameType.dlChunk.rawValue {
                var end = index
                while end < entries.count,
                      entries[end].frame.type == BuzzLinkFrameType.dlChunk.rawValue,
                      entries[end].direction == entry.direction {
                    end += 1
                }
                let segment = entries[index..<end]
                if segment.count >= 4 {
                    let first = segment.first!
                    let firstSeq = Self.chunkSeq(first.frame.payload)
                    let lastSeq = Self.chunkSeq(segment.last!.frame.payload)
                    rows.append(DisplayRow(
                        kind: .aggregate(firstRowID: first.rowID, count: segment.count,
                                         seqRange: "seq \(firstSeq)…\(lastSeq)"),
                        id: "a-\(first.rowID)"
                    ))
                    index = end
                    continue
                }
            }
            rows.append(DisplayRow(kind: .frame(entry), id: "f-\(entry.rowID)"))
            index += 1
        }
        return rows
    }

    var hasTypeFilter: Bool { !typeFilter.isEmpty }

    /// 展开态的完整帧 hex dump（offset + hex + ASCII 对照）
    func hexDump(for entry: FrameLogBuffer.Entry) -> String {
        Self.hexDump(bytes: [UInt8]((try? entry.frame.encoded()) ?? Data()))
    }

    /// 全量 17 种 type（拍板 A：下拉多选全列）
    static let allTypes: [UInt8] = [
        0x01, 0x02, 0x03, 0x04, 0x10, 0x11, 0x20, 0x21,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x40, 0x41, 0x7F
    ]

    static func typeName(_ type: UInt8) -> String { FrameLogBuffer.typeName(type) }

    // MARK: - 意图

    func setDirectionFilter(_ direction: FrameLogBuffer.Direction?) {
        directionFilter = direction
    }

    func toggleTypeFilter(_ type: UInt8) {
        if typeFilter.contains(type) {
            typeFilter.remove(type)
        } else {
            typeFilter.insert(type)
        }
    }

    func setTextFilter(_ text: String) {
        textFilter = text
    }

    func toggleExpanded(_ rowID: Int) {
        if expandedIDs.contains(rowID) {
            expandedIDs.remove(rowID)
        } else {
            expandedIDs.insert(rowID)
        }
    }

    func clear() {
        buffer.clear()
        expandedIDs.removeAll()
    }

    /// 预置命令发送；「开始下载」=拍板 A：BEGIN（等 RESP）→ 自动 ABORT，单键看全握手。
    func send(_ preset: Preset) {
        guard sendGate == nil else { return }
        Task { [weak self] in
            guard let self else { return }
            do {
                switch preset {
                case .list:
                    _ = try await self.sendRaw(type: BuzzLinkFrameType.listReq.rawValue,
                                               channel: BuzzLinkChannel.control.rawValue,
                                               payload: Data(#"{"offset":0}"#.utf8))
                case .info:
                    _ = try await self.sendRaw(type: BuzzLinkFrameType.infoGet.rawValue,
                                               channel: BuzzLinkChannel.control.rawValue,
                                               payload: Data())
                case .begin:
                    let payload = Self.fileIDPayload(self.demoFileID)
                    _ = try await self.sendRaw(type: BuzzLinkFrameType.dlBeginReq.rawValue,
                                               channel: BuzzLinkChannel.file.rawValue,
                                               payload: payload)
                    // 拍板 A：看完握手即释放链路
                    _ = try await self.sendRaw(type: BuzzLinkFrameType.dlAbort.rawValue,
                                               channel: BuzzLinkChannel.file.rawValue,
                                               payload: payload, awaitResponse: false)
                case .verify:
                    _ = try await self.sendRaw(type: BuzzLinkFrameType.dlEndReq.rawValue,
                                               channel: BuzzLinkChannel.file.rawValue,
                                               payload: Self.fileIDPayload(self.demoFileID))
                }
            } catch {
                self.appModel.showToast(ToastMessage(kind: .error, text: "发送失败：\(error.localizedDescription)"))
            }
        }
    }

    /// 自定义 hex 发送（拍板 A：3s 内同 session RX 高亮 + 自动展开）。
    /// - Returns: 校验失败原因（行内红字提示）；nil = 已发送。
    @discardableResult
    func sendCustomHex(_ raw: String) -> String? {
        guard sendGate == nil else { return nil }
        let stripped = raw.replacingOccurrences(of: "\\s+", with: "", options: .regularExpression)
        guard stripped.count % 2 == 0, stripped.allSatisfy(\.isHexDigit) else {
            return "hex 非法：须偶数长度且只含 0-9/a-f"
        }
        guard let bytes = Self.hexBytes(stripped) else { return "hex 解析失败" }
        guard bytes.count >= BuzzLinkConstants.frameOverhead,
              bytes[0] == BuzzLinkConstants.magicByte0, bytes[1] == BuzzLinkConstants.magicByte1 else {
            return "帧头非法：须以 BE EC 开头且 ≥\(BuzzLinkConstants.frameOverhead) 字节"
        }
        let type = bytes[3], flags = bytes[5]
        let channel = UInt16(bytes[6]) | (UInt16(bytes[7]) << 8)
        let length = Int(UInt16(bytes[8]) | (UInt16(bytes[9]) << 8))
        guard bytes.count >= BuzzLinkConstants.frameOverhead + length else {
            return "帧长度字段超出实际字节数"
        }
        let payload = Data(bytes[BuzzLinkConstants.frameOverhead..<(BuzzLinkConstants.frameOverhead + length)])
        Task { [weak self] in
            guard let self else { return }
            do {
                _ = try await self.sendRaw(type: type, flags: flags, channel: channel,
                                           payload: payload, awaitResponse: false, trackHighlight: true)
            } catch {
                self.appModel.showToast(ToastMessage(kind: .error, text: "发送失败：\(error.localizedDescription)"))
            }
        }
        return nil
    }

    /// 导出：`-consoleExportPath` 注入时直写（测试友好，跳过面板）；否则 NSSavePanel。
    func export() {
        let vector = buffer.exportVector(comment: "Beecord Studio 协议控制台导出")
        let data: Data
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        do {
            data = try encoder.encode(vector)
        } catch {
            appModel.showToast(ToastMessage(kind: .error, text: "导出编码失败：\(error.localizedDescription)"))
            return
        }
        if let injected = LaunchOptions.consoleExportPath {
            do {
                try data.write(to: URL(fileURLWithPath: injected), options: .atomic)
                appModel.showToast(ToastMessage(kind: .info, text: "已导出向量"))
            } catch {
                appModel.showToast(ToastMessage(kind: .error, text: "导出失败：\(error.localizedDescription)"))
            }
            return
        }
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "buzzlink-session-\(Int(Date().timeIntervalSince1970)).json"
        panel.allowedContentTypes = [.json]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try data.write(to: url, options: .atomic)
            appModel.showToast(ToastMessage(kind: .info, text: "已导出向量"))
        } catch {
            appModel.showToast(ToastMessage(kind: .error, text: "导出失败：\(error.localizedDescription)"))
        }
    }

    // MARK: - 私有

    private var demoFileID: String {
        deviceViewModel.snapshot.rows.first?.id ?? "REC_20260101_120000"
    }

    /// sendRaw 薄封装；awaitResponse=false 时经 allocatedSession 回调记录待高亮 session
    /// （拍板 A：自定义 hex 的响应反馈按同 session 匹配）。
    private func sendRaw(type: UInt8, flags: UInt8 = 0, channel: UInt16, payload: Data,
                         awaitResponse: Bool = true, trackHighlight: Bool = false) async throws -> BuzzLinkFrame? {
        let frame = BuzzLinkFrame(type: type, session: 0, flags: flags, channel: channel, payload: payload)
        return try await deviceViewModel.sendRawConsoleCommand(
            frame, awaitResponse: awaitResponse
        ) { [weak self] session in
            if trackHighlight {
                Task { @MainActor in
                    self?.pendingHighlightSession = session
                }
            }
        }
    }

    private func ingest(_ event: FrameTapEvent) async {
        let comment = FrameLogBuffer.typeName(event.frame.type)
        buffer.append(frame: event.frame, direction: event.direction, comment: comment)
        // 拍板 A：自定义 hex 发送后 3s 内同 session RX 高亮 + 自动展开
        if let pending = pendingHighlightSession,
           event.direction == .rx, event.frame.session == pending {
            pendingHighlightSession = nil
            let rowID = buffer.entries.last?.rowID
            highlightedRowID = rowID
            if let rowID { expandedIDs.insert(rowID) }
            highlightTask?.cancel()
            highlightTask = Task { [weak self] in
                try? await Task.sleep(for: .seconds(3))
                guard !Task.isCancelled else { return }
                self?.highlightedRowID = nil
            }
        }
    }

    /// DL_CHUNK payload 的 seq（前 2 字节 LE；不足 2 字节回退 0）
    private static func chunkSeq(_ payload: Data) -> UInt16 {
        guard payload.count >= 2 else { return 0 }
        return UInt16(payload[payload.startIndex]) | (UInt16(payload[payload.startIndex + 1]) << 8)
    }

    private static func fileIDPayload(_ fileID: String) -> Data {
        var data = Data([UInt8(fileID.utf8.count)])
        data.append(Data(fileID.utf8))
        return data
    }

    private static func hexBytes(_ stripped: String) -> [UInt8]? {
        var bytes: [UInt8] = []
        bytes.reserveCapacity(stripped.count / 2)
        var index = stripped.startIndex
        while index < stripped.endIndex {
            let next = stripped.index(index, offsetBy: 2)
            guard let byte = UInt8(stripped[index..<next], radix: 16) else { return nil }
            bytes.append(byte)
            index = next
        }
        return bytes
    }

    /// offset + hex（16/行）+ ASCII 对照
    static func hexDump(bytes: [UInt8]) -> String {
        var lines: [String] = []
        for offset in stride(from: 0, to: bytes.count, by: 16) {
            let chunk = Array(bytes[offset..<min(offset + 16, bytes.count)])
            let hex = chunk.map { String(format: "%02x", $0) }.joined(separator: " ")
                .padding(toLength: 16 * 3 - 1, withPad: " ", startingAt: 0)
            let ascii = chunk.map { (0x20..<0x7E).contains($0) ? String(Unicode.Scalar($0)) : "." }.joined()
            lines.append(String(format: "%04x  %@  %@", offset, hex, ascii))
        }
        return lines.joined(separator: "\n")
    }
}
