import BuzzLinkCore
import Foundation

/// 设备侧响应帧构造器：按 BUZZLINK.md §5/§8 生成编码后的线上字节，
/// 供 mock 传输回灌给客户端。JSON 形状与 BuzzLinkSimCore 输出同构。
enum TestResponseBytes {

    /// PONG（§5.1）
    static func pong(session: UInt8, payload: Data = Data()) throws -> Data {
        try BuzzLinkFrame(
            type: BuzzLinkFrameType.pong.rawValue,
            session: session,
            channel: BuzzLinkChannel.link.rawValue,
            payload: payload
        ).encoded()
    }

    /// INFO（§8.1）
    static func info(
        session: UInt8,
        deviceName: String = "BCD-TEST",
        battery: Int = 87
    ) throws -> Data {
        let json = """
        {"model":"beecord-sim","fwVersion":"0.1.0","protocol":1,\
        "deviceName":"\(deviceName)","battery":\(battery),"charging":false,\
        "storageTotal":31000000,"storageFree":12345678}
        """
        return try BuzzLinkFrame(
            type: BuzzLinkFrameType.info.rawValue,
            session: session,
            channel: BuzzLinkChannel.control.rawValue,
            payload: Data(json.utf8)
        ).encoded()
    }

    /// LIST_RESP 成功页（§8.2）
    static func listResp(
        session: UInt8,
        total: Int,
        offset: Int,
        entries: [(id: String, durationMs: UInt32, size: Int)]
    ) throws -> Data {
        let files = entries
            .map { "{\"id\":\"\($0.id)\",\"durationMs\":\($0.durationMs),\"size\":\($0.size)}" }
            .joined(separator: ",")
        let json = "{\"status\":\"ok\",\"total\":\(total),\"offset\":\(offset),\"files\":[\(files)]}"
        return try BuzzLinkFrame(
            type: BuzzLinkFrameType.listResp.rawValue,
            session: session,
            channel: BuzzLinkChannel.control.rawValue,
            payload: Data(json.utf8)
        ).encoded()
    }

    /// 承载在响应 type 里的错误 JSON（§8.2/§8.3 错误形态）
    static func jsonError(
        type: BuzzLinkFrameType,
        session: UInt8,
        code: Int,
        message: String
    ) throws -> Data {
        let json = "{\"status\":\"error\",\"code\":\(code),\"message\":\"\(message)\"}"
        return try BuzzLinkFrame(
            type: type.rawValue,
            session: session,
            channel: BuzzLinkChannel.control.rawValue,
            payload: Data(json.utf8)
        ).encoded()
    }

    /// ERROR 帧（§9）
    static func errorFrame(session: UInt8, code: Int, message: String) throws -> Data {
        let json = "{\"status\":\"error\",\"code\":\(code),\"message\":\"\(message)\"}"
        return try BuzzLinkFrame(
            type: BuzzLinkFrameType.error.rawValue,
            session: session,
            channel: BuzzLinkChannel.control.rawValue,
            payload: Data(json.utf8)
        ).encoded()
    }

    /// FILE_INFO_RESP 成功（§8.3）
    static func fileInfoResp(
        session: UInt8,
        id: String,
        size: Int,
        crc16: UInt16,
        durationMs: UInt32 = 1_000
    ) throws -> Data {
        let json = """
        {"status":"ok","id":"\(id)","size":\(size),"crc16":\(crc16),\
        "durationMs":\(durationMs),"format":"ogg-opus","sampleRate":16000,"channels":1}
        """
        return try BuzzLinkFrame(
            type: BuzzLinkFrameType.fileInfoResp.rawValue,
            session: session,
            channel: BuzzLinkChannel.control.rawValue,
            payload: Data(json.utf8)
        ).encoded()
    }

    /// DL_BEGIN_RESP 13 字节定长（§5.1）
    static func dlBeginResp(
        session: UInt8,
        status: BuzzLinkStatus = .ok,
        chunkDataLen: UInt16 = UInt16(BuzzLinkConstants.defaultChunkDataLength),
        totalChunks: UInt32,
        size: UInt32,
        fileCRC16: UInt16
    ) throws -> Data {
        var payload = Data()
        payload.append(status.rawValue)
        payload.appendClientLE16(chunkDataLen)
        payload.appendClientLE32(totalChunks)
        payload.appendClientLE32(size)
        payload.appendClientLE16(fileCRC16)
        return try BuzzLinkFrame(
            type: BuzzLinkFrameType.dlBeginResp.rawValue,
            session: session,
            channel: BuzzLinkChannel.file.rawValue,
            payload: payload
        ).encoded()
    }

    /// DL_CHUNK（§5.1）
    static func dlChunk(session: UInt8, seq: UInt16, data: Data) throws -> Data {
        var payload = Data()
        payload.appendClientLE16(seq)
        payload.append(data)
        return try BuzzLinkFrame(
            type: BuzzLinkFrameType.dlChunk.rawValue,
            session: session,
            channel: BuzzLinkChannel.file.rawValue,
            payload: payload
        ).encoded()
    }

    /// DL_END_RESP 3 字节定长（§5.1）
    static func dlEndResp(
        session: UInt8,
        status: BuzzLinkStatus = .ok,
        fileCRC16: UInt16
    ) throws -> Data {
        var payload = Data()
        payload.append(status.rawValue)
        payload.appendClientLE16(fileCRC16)
        return try BuzzLinkFrame(
            type: BuzzLinkFrameType.dlEndResp.rawValue,
            session: session,
            channel: BuzzLinkChannel.file.rawValue,
            payload: payload
        ).encoded()
    }
}

// MARK: - 小端追加（测试私有 helper，与生产实现互不依赖）

private extension Data {
    mutating func appendClientLE16(_ value: UInt16) {
        append(UInt8(value & 0xFF))
        append(UInt8((value >> 8) & 0xFF))
    }

    mutating func appendClientLE32(_ value: UInt32) {
        append(UInt8(value & 0xFF))
        append(UInt8((value >> 8) & 0xFF))
        append(UInt8((value >> 16) & 0xFF))
        append(UInt8((value >> 24) & 0xFF))
    }
}
