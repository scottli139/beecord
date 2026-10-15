import BuzzLinkCore
import BuzzLinkSimCore
import Foundation
import XCTest

/// 测试辅助：构造请求帧、把引擎输出编码为线上字节并回放给"客户端侧"重组链。
enum SimTestSupport {

    /// 确定性内容的虚拟文件
    static func makeFile(id: String, durationMs: UInt32 = 1_000, size: Int, seed: UInt64 = 7) -> SimFile {
        var rng = SplitMix64(seed: seed)
        return SimFile(
            id: id,
            durationMs: durationMs,
            data: Data((0..<size).map { _ in UInt8(truncatingIfNeeded: rng.next()) })
        )
    }

    /// 控制通道 JSON 请求帧
    static func controlRequest(type: BuzzLinkFrameType, session: UInt8, json: String) -> BuzzLinkFrame {
        BuzzLinkFrame(
            type: type.rawValue,
            session: session,
            channel: BuzzLinkChannel.control.rawValue,
            payload: Data(json.utf8)
        )
    }

    /// 文件通道二进制请求帧
    static func fileRequest(type: BuzzLinkFrameType, session: UInt8, payload: Data) -> BuzzLinkFrame {
        BuzzLinkFrame(
            type: type.rawValue,
            session: session,
            channel: BuzzLinkChannel.file.rawValue,
            payload: payload
        )
    }

    /// `[idLen u8][id…]` payload（§5.1）
    static func idPayload(_ id: String) -> Data {
        var payload = Data([UInt8(id.utf8.count)])
        payload.append(Data(id.utf8))
        return payload
    }

    /// 引擎输出 → 线上字节（应用 corruptCRC 故障：翻转 CRC 字段）
    static func wireBytes(_ outbound: SimOutboundFrame) throws -> Data {
        var bytes = try outbound.frame.encoded()
        if outbound.corruptCRC {
            let lastIndex = bytes.index(before: bytes.endIndex)
            bytes[lastIndex] ^= 0xFF
        }
        return bytes
    }

    static func decodeJSONObject(_ frame: BuzzLinkFrame, file: StaticString = #filePath, line: UInt = #line) throws -> [String: Any] {
        let object = try JSONSerialization.jsonObject(with: frame.payload)
        return try XCTUnwrap(object as? [String: Any], "payload 应为 JSON 对象", file: file, line: line)
    }
}

/// 客户端侧接收链：线上字节 → 字节流重组 → 分片重组 → 完整消息帧。
/// 复用 BuzzLinkCore 的实现，验证 Sim 输出能被规范接收方完整解析。
final class ClientHarness {
    private var parser = BuzzLinkFrameParser()
    private var reassembler = BuzzLinkMessageReassembler()

    var discardedFrameCount: Int { parser.discardedFrameCount }

    @discardableResult
    func receive(_ outbound: [SimOutboundFrame]) throws -> [BuzzLinkFrame] {
        var messages: [BuzzLinkFrame] = []
        for item in outbound {
            parser.append(try SimTestSupport.wireBytes(item))
            while let frame = parser.nextFrame() {
                if let message = try reassembler.accept(frame) {
                    messages.append(message)
                }
            }
        }
        return messages
    }

    @discardableResult
    func receive(_ outbound: SimOutboundFrame?) throws -> [BuzzLinkFrame] {
        try receive(outbound.map { [$0] } ?? [])
    }
}
