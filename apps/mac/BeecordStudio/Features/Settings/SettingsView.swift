import AppKit
import BuzzLinkCore
import Foundation
import os
import SwiftUI

/// 设置窗口：通用 / AI 服务 / 转写（P1 占位，整组 disabled）/ 关于。
/// 字段真实持久化：AppStorage（key 前缀 `settings.`）+ API Key 走 Keychain。
struct SettingsView: View {
    @Bindable var settingsModel: SettingsModel

    private let logger = Logger(subsystem: "com.beeuc.beecord.studio", category: "settings")

    // 转写组为 P1 占位（整组 disabled），暂用视图本地态。
    @State private var whisperModel = 0
    @State private var whisperLanguage = 0

    var body: some View {
        Form {
            generalSection
            aiSection
            transcriptionSection
            aboutSection
        }
        .formStyle(.grouped)
        .frame(width: 640)
    }

    // MARK: - 通用

    private var generalSection: some View {
        Section("通用") {
            Picker("连接方式", selection: Binding(
                get: { TransportMode(rawValue: settingsModel.transportMode) ?? .loopback },
                set: { settingsModel.setTransportMode($0) }
            )) {
                ForEach(TransportMode.allCases) { mode in
                    Text(mode.displayName).tag(mode)
                }
            }
            .accessibilityIdentifier("settings.transportPicker")
            LabeledContent("下载目录") {
                HStack(spacing: DesignTokens.Space.s2) {
                    Text(settingsModel.downloadDir.isEmpty ? "未选择" : settingsModel.downloadDir)
                        .foregroundStyle(settingsModel.downloadDir.isEmpty ? DesignTokens.ColorToken.textTertiary : DesignTokens.ColorToken.textPrimary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .frame(maxWidth: 320, alignment: .trailing)
                        .accessibilityIdentifier("settings.downloadDir")
                    Button("选择…") {
                        chooseDownloadDirectory()
                    }
                    .accessibilityIdentifier("settings.downloadDir.chooseButton")
                }
            }
            Toggle("自动同步", isOn: Binding(
                get: { settingsModel.autoSyncEnabled },
                set: { settingsModel.setAutoSyncEnabled($0) }
            ))
            .accessibilityIdentifier("settings.autoSyncToggle")
            Toggle("开发者模式", isOn: Binding(
                get: { settingsModel.developerMode },
                set: { settingsModel.setDeveloperMode($0) }
            ))
            .accessibilityIdentifier("settings.developerModeToggle")
            Text("开启后在侧栏显示协议控制台。")
                .font(DesignTokens.FontToken.caption)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
        }
    }

    // MARK: - AI 服务

    private var aiSection: some View {
        Section("AI 服务") {
            Picker("服务商", selection: Binding(
                get: { settingsModel.aiProvider },
                set: { settingsModel.setAIProvider($0) }
            )) {
                ForEach(AIProvider.allCases) { provider in
                    Text(provider.displayName).tag(provider)
                }
            }
            .accessibilityIdentifier("settings.aiProviderPicker")
            LabeledContent("Base URL") {
                TextField("", text: Binding(
                    get: { settingsModel.aiBaseURL },
                    set: { settingsModel.setAIBaseURL($0) }
                ))
                .textFieldStyle(.roundedBorder)
                .frame(maxWidth: 320)
                .accessibilityIdentifier("settings.aiBaseURL")
            }
            LabeledContent("API Key") {
                SecureField("", text: Binding(
                    get: { settingsModel.apiKey },
                    set: { settingsModel.setAPIKey($0) }
                ))
                .textFieldStyle(.roundedBorder)
                .frame(maxWidth: 320)
                .accessibilityIdentifier("settings.aiAPIKey")
            }
            LabeledContent("模型") {
                TextField("", text: Binding(
                    get: { settingsModel.aiModel },
                    set: { settingsModel.setAIModel($0) }
                ))
                .textFieldStyle(.roundedBorder)
                .frame(maxWidth: 320)
                .accessibilityIdentifier("settings.aiModel")
            }
            Section {
                Toggle("生成摘要", isOn: Binding(
                    get: { settingsModel.aiFeatureSummary },
                    set: { settingsModel.setAIFeatureSummary($0) }
                ))
                .accessibilityIdentifier("settings.aiFeature.summary")
                Toggle("会议纪要", isOn: Binding(
                    get: { settingsModel.aiFeatureMeeting },
                    set: { settingsModel.setAIFeatureMeeting($0) }
                ))
                .accessibilityIdentifier("settings.aiFeature.meeting")
                Toggle("待办提取", isOn: Binding(
                    get: { settingsModel.aiFeatureTodo },
                    set: { settingsModel.setAIFeatureTodo($0) }
                ))
                .accessibilityIdentifier("settings.aiFeature.todo")
                Toggle("翻译", isOn: Binding(
                    get: { settingsModel.aiFeatureTranslate },
                    set: { settingsModel.setAIFeatureTranslate($0) }
                ))
                .accessibilityIdentifier("settings.aiFeature.translate")
            } header: {
                Text("功能")
            }
            HStack {
                Spacer()
                Button("测试连接") {
                    // P1（T10）接入真实连通性测试
                }
                .disabled(true)
                .help("P1 可用")
                .accessibilityIdentifier("settings.aiTestButton")
            }
        }
    }

    // MARK: - 转写（P1 占位，整组 disabled）

    private var transcriptionSection: some View {
        Section("转写") {
            Picker("模型", selection: $whisperModel) {
                Text("ggml-small").tag(0)
                Text("ggml-base").tag(1)
                Text("ggml-tiny").tag(2)
            }
            .accessibilityIdentifier("settings.whisper.model")
            HStack {
                Spacer()
                Button("管理模型…") {
                    // P1（T9）打开模型管理
                }
                .accessibilityIdentifier("settings.whisper.manageButton")
            }
            Picker("语言", selection: $whisperLanguage) {
                Text("自动检测").tag(0)
                Text("中文").tag(1)
                Text("English").tag(2)
            }
            .accessibilityIdentifier("settings.whisper.language")
        }
        .disabled(true)
    }

    // MARK: - 关于

    private var aboutSection: some View {
        Section("关于") {
            LabeledContent("版本", value: appVersion)
                .accessibilityIdentifier("settings.about.version")
            LabeledContent("协议", value: "BuzzLink \(protocolVersion)")
                .accessibilityIdentifier("settings.about.protocolVersion")
            LabeledContent("许可证", value: "Apache-2.0")
                .accessibilityIdentifier("settings.about.license")
            Text("本地优先：录音与转写默认留在本机，仅在设备同步时经你许可传输。")
                .font(DesignTokens.FontToken.caption)
                .foregroundStyle(DesignTokens.ColorToken.textSecondary)
                .accessibilityIdentifier("settings.about.tagline")
        }
    }

    // MARK: - 值

    private var appVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.1.0"
    }

    private var protocolVersion: String {
        String(format: "0x%02X", BuzzLinkConstants.version)
    }

    // MARK: - 动作

    private func chooseDownloadDirectory() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.message = "选择录音下载目录"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        guard let bookmark = SecurityScopedBookmark.makeBookmark(for: url) else {
            logger.error("下载目录安全书签生成失败：\(url.path)")
            return
        }
        settingsModel.setDownloadDirBookmark(bookmark)
        settingsModel.setDownloadDir(url.path)
    }
}

#Preview("浅色") {
    SettingsView(settingsModel: SettingsModel())
}

#Preview("深色") {
    SettingsView(settingsModel: SettingsModel())
        .preferredColorScheme(.dark)
}
