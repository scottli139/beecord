import Foundation
import os

/// 设置页模型：UserDefaults 持久化（key 统一前缀 `settings.`）+ API Key 走 Keychain。
/// 存储为普通属性 + didSet 直写（@Observable 不支持属性包装器；didSet 保证观测与持久化同时生效）。
@MainActor
@Observable
final class SettingsModel {

    /// UserDefaults key 常量（前缀 `settings.`）。
    enum Keys {
        static let downloadDir = "settings.downloadDir"
        static let autoSyncEnabled = "settings.autoSyncEnabled"
        static let developerMode = "settings.developerMode"
        static let aiProvider = "settings.aiProvider"
        static let aiBaseURL = "settings.aiBaseURL"
        static let aiModel = "settings.aiModel"
        static let aiFeatureSummary = "settings.aiFeature.summary"
        static let aiFeatureMeeting = "settings.aiFeature.meeting"
        static let aiFeatureTodo = "settings.aiFeature.todo"
        static let aiFeatureTranslate = "settings.aiFeature.translate"
    }

    // MARK: - 通用

    /// 下载目录路径字符串（T4 仅存路径；安全书签 T5 再引入）。
    var downloadDir: String { didSet { Self.store(downloadDir, forKey: Keys.downloadDir) } }
    /// 自动同步开关（默认开）。
    var autoSyncEnabled: Bool { didSet { Self.store(autoSyncEnabled, forKey: Keys.autoSyncEnabled) } }
    /// 开发者模式（默认关）：开启后侧栏显示协议控制台。
    var developerMode: Bool { didSet { Self.store(developerMode, forKey: Keys.developerMode) } }

    // MARK: - AI 服务

    /// 服务商预设 raw 值（默认 Kimi）。
    var aiProviderRaw: String { didSet { Self.store(aiProviderRaw, forKey: Keys.aiProvider) } }
    var aiBaseURL: String { didSet { Self.store(aiBaseURL, forKey: Keys.aiBaseURL) } }
    var aiModel: String { didSet { Self.store(aiModel, forKey: Keys.aiModel) } }
    var aiFeatureSummary: Bool { didSet { Self.store(aiFeatureSummary, forKey: Keys.aiFeatureSummary) } }
    var aiFeatureMeeting: Bool { didSet { Self.store(aiFeatureMeeting, forKey: Keys.aiFeatureMeeting) } }
    var aiFeatureTodo: Bool { didSet { Self.store(aiFeatureTodo, forKey: Keys.aiFeatureTodo) } }
    var aiFeatureTranslate: Bool { didSet { Self.store(aiFeatureTranslate, forKey: Keys.aiFeatureTranslate) } }

    /// AI API Key（内存态；持久化走 KeychainStore，不落 UserDefaults）。
    private(set) var apiKey = ""

    private let logger = Logger(subsystem: "com.beeuc.beecord.studio", category: "settings")
    private let keychainService = "com.beeuc.beecord.studio.ai"
    private let keychainAccount = "apikey"

    init() {
        let defaults = UserDefaults.standard
        downloadDir = defaults.string(forKey: Keys.downloadDir) ?? ""
        autoSyncEnabled = defaults.object(forKey: Keys.autoSyncEnabled) as? Bool ?? true
        developerMode = defaults.object(forKey: Keys.developerMode) as? Bool ?? false
        aiProviderRaw = defaults.string(forKey: Keys.aiProvider) ?? AIProvider.kimi.rawValue
        aiBaseURL = defaults.string(forKey: Keys.aiBaseURL) ?? AIProvider.kimi.defaultBaseURL
        aiModel = defaults.string(forKey: Keys.aiModel) ?? AIProvider.kimi.defaultModel
        aiFeatureSummary = defaults.object(forKey: Keys.aiFeatureSummary) as? Bool ?? true
        aiFeatureMeeting = defaults.object(forKey: Keys.aiFeatureMeeting) as? Bool ?? true
        aiFeatureTodo = defaults.object(forKey: Keys.aiFeatureTodo) as? Bool ?? true
        aiFeatureTranslate = defaults.object(forKey: Keys.aiFeatureTranslate) as? Bool ?? false
        apiKey = (try? KeychainStore.read(service: keychainService, account: keychainAccount)) ?? ""
    }

    // MARK: - 意图方法

    var aiProvider: AIProvider {
        get { AIProvider(rawValue: aiProviderRaw) ?? .kimi }
        set {
            aiProviderRaw = newValue.rawValue
            // 切换预设自动带出 baseURL + model（可再改）
            aiBaseURL = newValue.defaultBaseURL
            aiModel = newValue.defaultModel
        }
    }

    func setDownloadDir(_ path: String) { downloadDir = path }
    func setAutoSyncEnabled(_ value: Bool) { autoSyncEnabled = value }
    func setDeveloperMode(_ value: Bool) { developerMode = value }
    func setAIProvider(_ provider: AIProvider) { aiProvider = provider }
    func setAIBaseURL(_ value: String) { aiBaseURL = value }
    func setAIModel(_ value: String) { aiModel = value }
    func setAIFeatureSummary(_ value: Bool) { aiFeatureSummary = value }
    func setAIFeatureMeeting(_ value: Bool) { aiFeatureMeeting = value }
    func setAIFeatureTodo(_ value: Bool) { aiFeatureTodo = value }
    func setAIFeatureTranslate(_ value: Bool) { aiFeatureTranslate = value }

    /// 更新 API Key 并同步写入 Keychain；空串视为清除。
    func setAPIKey(_ value: String) {
        apiKey = value
        do {
            try KeychainStore.write(value, service: keychainService, account: keychainAccount)
        } catch {
            logger.error("API Key 写入 Keychain 失败：\(error.localizedDescription)")
        }
    }

    // MARK: - 持久化

    private nonisolated static func store(_ value: Any?, forKey key: String) {
        UserDefaults.standard.set(value, forKey: key)
    }
}
