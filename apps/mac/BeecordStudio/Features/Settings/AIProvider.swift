/// AI 服务商预设（ux-design §2：Kimi / DeepSeek / Qwen + 自定义 OpenAI 兼容端点）。
enum AIProvider: String, CaseIterable, Identifiable {
    case kimi
    case deepseek
    case qwen
    case custom

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .kimi: "Kimi"
        case .deepseek: "DeepSeek"
        case .qwen: "通义千问"
        case .custom: "自定义"
        }
    }

    /// 切换预设时自动带出的默认 Base URL（可再改）。
    var defaultBaseURL: String {
        switch self {
        case .kimi: "https://api.moonshot.cn/v1"
        case .deepseek: "https://api.deepseek.com/v1"
        case .qwen: "https://dashscope.aliyuncs.com/compatible-mode/v1"
        case .custom: ""
        }
    }

    /// 切换预设时自动带出的默认模型（可再改）。
    var defaultModel: String {
        switch self {
        case .kimi: "kimi-k3"
        case .deepseek: "deepseek-chat"
        case .qwen: "qwen-plus"
        case .custom: ""
        }
    }
}
