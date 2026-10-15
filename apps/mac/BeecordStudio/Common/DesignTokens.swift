import SwiftUI

/// 设计令牌子集（T4 骨架落地），值取自 DESIGN_SYSTEM.md §1。
/// Mac 端优先系统语义色（自动适配深浅色）；品牌令牌仅在规定的品牌点使用。
enum DesignTokens {

    /// 间距刻度（§1.3，4 的倍数）。
    enum Space {
        /// space.1 = 4
        static let s1: CGFloat = 4
        /// space.2 = 8
        static let s2: CGFloat = 8
        /// space.3 = 12
        static let s3: CGFloat = 12
        /// space.4 = 16
        static let s4: CGFloat = 16
        /// space.5 = 20
        static let s5: CGFloat = 20
        /// space.6 = 24
        static let s6: CGFloat = 24
        /// space.8 = 32
        static let s8: CGFloat = 32
    }

    /// 圆角刻度（§1.3）。
    enum Radius {
        /// radius.sm = 6（Mac 控件）
        static let sm: CGFloat = 6
        /// radius.md = 10（Mac 卡片）
        static let md: CGFloat = 10
        /// radius.lg = 14
        static let lg: CGFloat = 14
        /// radius.xl = 20（浮层卡片）
        static let xl: CGFloat = 20
    }

    /// 主区页面内边距（§1.3：22×26）。
    enum Layout {
        static let mainPaddingHorizontal: CGFloat = 26
        static let mainPaddingVertical: CGFloat = 22
        /// 侧栏宽 216（可调 180~240）。
        static let sidebarMin: CGFloat = 180
        static let sidebarIdeal: CGFloat = 216
        static let sidebarMax: CGFloat = 240
        /// 窗口默认 1060×720，最小 960×600。
        static let windowMinWidth: CGFloat = 960
        static let windowMinHeight: CGFloat = 600
        static let windowIdealWidth: CGFloat = 1060
        static let windowIdealHeight: CGFloat = 720
    }

    /// 语义色（§1.1）。surface.card/card2 与 scrim 为双外观显式值，其余用系统语义色。
    enum ColorToken {
        /// surface.card：Light #FFFFFF / Dark #1C1C1E
        static let surfaceCard = Color(light: 0xFFFFFF, dark: 0x1C1C1E)
        /// surface.card2（次级填充 / 轨道）：Light #F5F5F7 / Dark #2C2C2E
        static let surfaceCard2 = Color(light: 0xF5F5F7, dark: 0x2C2C2E)
        /// scrim.overlay：Light rgba(0,0,0,.35) / Dark rgba(0,0,0,.45)
        static let scrim = Color(light: 0x000000, dark: 0x000000, lightOpacity: 0.35, darkOpacity: 0.45)
        /// brand.primary #F5A623（仅品牌点；琥珀底上的文字用墨黑 #101014）
        static let brandPrimary = Color(.sRGB, red: 0xF5 / 255, green: 0xA6 / 255, blue: 0x23 / 255, opacity: 1)
        /// 墨黑：琥珀底上的文字专用
        static let brandInk = Color(.sRGB, red: 0x10 / 255, green: 0x10 / 255, blue: 0x14 / 255, opacity: 1)
        /// state.recording 录音态 / REC 指示
        static let stateRecording = Color(.systemRed)
        /// state.sync 连接 / 同步（systemBlue）
        static let stateSync = Color(.systemBlue)
        /// state.success 校验通过 / 完成
        static let stateSuccess = Color(.systemGreen)
        /// state.warning 低电 / 告警
        static let stateWarning = Color(.systemOrange)
        /// state.danger 删除 / 错误
        static let stateDanger = Color(.systemRed)
        /// text.primary
        static let textPrimary = Color(.labelColor)
        /// text.secondary
        static let textSecondary = Color(.secondaryLabelColor)
        /// text.tertiary
        static let textTertiary = Color(.tertiaryLabelColor)
    }

    /// 字体（§1.2：一律系统样式，不写死字体名；等宽为 Menlo）。
    enum FontToken {
        /// 页面标题 22/700
        static let pageTitle = Font.title2.bold()
        /// 分组标题 13/600
        static let groupTitle = Font.headline
        /// 正文 13/400
        static let body = Font.body
        /// 辅助 12/400
        static let callout = Font.callout
        /// 弱提示 11/400
        static let caption = Font.caption
        /// 等宽（Menlo 12）：控制台 hex、帧日志
        static let mono = Font.system(size: 12, design: .monospaced)
        /// 数字等宽：计时、进度、数值（禁跳动变宽）
        static let digit = Font.body.monospacedDigit()
        /// 空态标题 15/600（DESIGN_SYSTEM §2 空态规格）
        static let emptyStateTitle = Font.system(size: 15, weight: .semibold)
    }

    /// 动效时长（§1.4，秒）。
    enum Motion {
        /// motion.micro = 100ms（按压反馈、开关）
        static let micro: TimeInterval = 0.1
        /// motion.fast = 150ms（toast 进出、hover）
        static let fast: TimeInterval = 0.15
        /// motion.normal = 250ms（页面切换、浮层）
        static let normal: TimeInterval = 0.25
    }
}

extension Color {
    /// 双外观显式色值（仅用于 surface/scrim 等系统无对应语义色的令牌）。
    fileprivate init(light lightHex: UInt32, dark darkHex: UInt32, lightOpacity: Double = 1, darkOpacity: Double = 1) {
        self.init(nsColor: NSColor(name: nil) { appearance in
            let isDark = appearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
            let hex = isDark ? darkHex : lightHex
            let opacity = isDark ? darkOpacity : lightOpacity
            return NSColor(
                srgbRed: CGFloat((hex >> 16) & 0xFF) / 255,
                green: CGFloat((hex >> 8) & 0xFF) / 255,
                blue: CGFloat(hex & 0xFF) / 255,
                alpha: opacity
            )
        })
    }
}
