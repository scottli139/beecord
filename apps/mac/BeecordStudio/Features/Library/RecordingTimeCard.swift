import BuzzLinkSync
import SwiftUI

struct RecordingTimeCard: View {
    let meta: RecordingMeta
    let isEditing: Bool
    @Binding var draft: Date
    let onEdit: () -> Void
    let onSave: () -> Void
    let onCancel: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: DesignTokens.Space.s3) {
            Grid(alignment: .leading, horizontalSpacing: DesignTokens.Space.s3,
                 verticalSpacing: DesignTokens.Space.s2) {
                GridRow {
                    Text("录制时间")
                        .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                    Text(recordedAtText)
                        .foregroundStyle(meta.recordedAt == nil
                                         ? DesignTokens.ColorToken.textSecondary
                                         : DesignTokens.ColorToken.textPrimary)
                        .accessibilityIdentifier("library.recordedAt.value")
                    Button(meta.recordedAt == nil ? "设置时间" : "修改", action: onEdit)
                        .accessibilityIdentifier("library.recordedAt.editButton")
                }
                GridRow {
                    Text("导入时间")
                        .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                    Text(Self.dateText(meta.importedAt))
                        .accessibilityIdentifier("library.importedAt.value")
                    Text("")
                }
            }
            .font(DesignTokens.FontToken.callout)

            Text(noteText)
                .font(DesignTokens.FontToken.caption)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                .fixedSize(horizontal: false, vertical: true)

            if isEditing {
                Divider()
                HStack(spacing: DesignTokens.Space.s2) {
                    DatePicker("录制日期", selection: $draft, displayedComponents: .date)
                        .labelsHidden()
                        .accessibilityLabel("录制日期")
                        .accessibilityIdentifier("library.recordedAt.dateField")
                    DatePicker("录制时间", selection: $draft, displayedComponents: .hourAndMinute)
                        .labelsHidden()
                        .accessibilityLabel("录制时间")
                        .accessibilityIdentifier("library.recordedAt.timeField")
                    Button("保存", action: onSave)
                        .buttonStyle(.borderedProminent)
                        .accessibilityIdentifier("library.recordedAt.saveButton")
                    Button("取消", action: onCancel)
                        .accessibilityIdentifier("library.recordedAt.cancelButton")
                }
                .accessibilityElement(children: .contain)
                .accessibilityIdentifier("library.recordedAt.editor")
                Text("保存后标注“手动”；录音标题保持不变。")
                    .font(DesignTokens.FontToken.caption)
                    .foregroundStyle(DesignTokens.ColorToken.textSecondary)
            }
        }
        .padding(DesignTokens.Space.s4)
        .cardStyle()
    }

    private var recordedAtText: String {
        guard let recordedAt = meta.recordedAt else { return "未知" }
        let text = Self.dateText(
            recordedAt,
            utcOffsetMin: meta.recordedAtUtcOffsetMin
        )
        switch meta.recordedAtSource {
        case .manual:
            return text + "（手动）"
        case .filename:
            return text + "（推断）"
        default:
            return text
        }
    }

    private var noteText: String {
        switch meta.recordedAtSource {
        case .manual:
            return "时间由你手动设置，仅保存在此 Mac；不写回手表，后续同步不会覆盖人工值。"
        case .device:
            return "时间来自设备元数据，可手动修正；修正后只保存在此 Mac，不写回手表。"
        case .filename:
            return "时间由旧文件名推断，可手动修正；导入时间不代表录制时间。"
        default:
            return "旧录音没有可信起录时间；导入时间不能代替录制时间。手动修正只修改此 Mac 的元数据，不写回手表，后续同步不会覆盖人工值。"
        }
    }

    private static func dateText(_ date: Date, utcOffsetMin: Int? = nil) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        if let utcOffsetMin, let zone = TimeZone(secondsFromGMT: utcOffsetMin * 60) {
            formatter.timeZone = zone
        }
        formatter.dateFormat = "yyyy-MM-dd HH:mm"
        return formatter.string(from: date)
    }
}
