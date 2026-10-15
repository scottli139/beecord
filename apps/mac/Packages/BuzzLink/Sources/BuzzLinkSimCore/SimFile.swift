import BuzzLinkCore
import Foundation

/// 虚拟录音文件（Sim 设备文件库条目）。
///
/// `data` 为完整文件内容：App 侧使用合成 Ogg Opus 样本（tools/make_sim_samples.sh 生成），
/// 测试可用任意字节——协议层只关心长度与 CRC。
public struct SimFile: Equatable, Sendable, Identifiable {
    /// 文件 ID（UTF-8 不透明字符串，≤ ``BuzzLinkConstants/maxFileIDLength`` 字节，§5.2）
    public var id: String
    /// 录音时长（毫秒）
    public var durationMs: UInt32
    /// 文件完整内容
    public var data: Data

    public init(id: String, durationMs: UInt32, data: Data) {
        self.id = id
        self.durationMs = durationMs
        self.data = data
    }

    /// 整文件 CRC-16/CCITT-FALSE（§6.3，下载验收用）
    public var crc16: UInt16 {
        CRC16.ccittFalse(data)
    }

    /// 故障注入用异常文件名条目（TESTING.md §5"超长文件名 / 非法字符"）：
    /// 一个含路径分隔符与 shell 特殊字符，一个恰为 32 字节上限的长中文名。
    /// 内容为确定性伪随机字节（占位即可，客户端入库安全化与 UI 截断是测试目标）。
    public static func hostileEntries() -> [SimFile] {
        var rng = SplitMix64(seed: 0xBEEC_0001)
        let data = Data((0..<1024).map { _ in UInt8(truncatingIfNeeded: rng.next()) })
        return [
            SimFile(id: "REC_2026/06:06*?", durationMs: 1_000, data: Data(data.prefix(256))),
            SimFile(id: "REC_超长文件名截断测ABCD", durationMs: 2_000, data: data)
        ]
    }
}
