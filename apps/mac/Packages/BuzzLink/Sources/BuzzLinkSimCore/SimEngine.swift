import BuzzLinkCore
import Foundation

/// 一帧待发送数据。
public struct SimOutboundFrame: Equatable, Sendable {
    /// 协议帧（合法帧；故障损坏由壳层在编码后施加）
    public var frame: BuzzLinkFrame
    /// 故障注入（TESTING.md §5 末帧 CRC 错误）：壳层在 `encoded()` 之后翻转 CRC 字段
    public var corruptCRC: Bool

    public init(frame: BuzzLinkFrame, corruptCRC: Bool = false) {
        self.frame = frame
        self.corruptCRC = corruptCRC
    }
}

/// 进行中的下载推流状态（BUZZLINK.md §6.2）。
public struct SimStreamState: Equatable, Sendable {
    /// 下载目标文件 ID
    public var fileID: String
    /// 沿用 DL_BEGIN 的 session（§3.4）
    public var session: UInt8
    /// 设备声明的块数据长度（≤ ``BuzzLinkConstants/maxChunkDataLength``）
    public var chunkDataLen: Int
    /// 总块数
    public var totalChunks: Int
    /// 下一块 seq（已决策未发送的块也算在内；丢块只跳发送不跳序号）
    public var nextSeq: Int
    /// 实际已发出的块数（丢块 < nextSeq）
    public var sentChunks: Int
    /// 本轮是否已触发过中途静默
    public var didEnterSilence: Bool
}

/// BuzzLink 虚拟设备协议引擎（BUZZLINK.md §12：严格按规范应答，
/// 故障注入不改变合法路径行为）。
///
/// 纯逻辑、无 CoreBluetooth 依赖：输入为已完成分片重组的消息帧（``handle(_:now:)``），
/// 输出为待发送帧；文件推流采用**拉取模型**——壳层循环调 ``nextChunk(now:)`` 并负责
/// 编码、限速节奏与 GATT 背压。时间由调用方注入（单调秒），测试可精确控制。
public struct SimEngine: Sendable {

    /// 设备档案（INFO 来源；``SimFaults/storageNearlyFull`` 会在上报时覆盖剩余容量）
    public var profile: SimDeviceProfile
    /// 故障注入开关
    public var faults: SimFaults
    /// 文件库（按文件 ID 降序 = 起录时间新→旧，§5.2）
    public private(set) var files: [SimFile]
    /// 进行中的推流；nil = 空闲
    public private(set) var activeStream: SimStreamState?

    /// 累计收到的消息帧数
    public private(set) var receivedFrameCount = 0
    /// 累计发出的帧数（含推流块）
    public private(set) var sentFrameCount = 0
    /// 故障注入累计丢弃的块数
    public private(set) var droppedChunkCount = 0

    /// 中途静默触发点：推流进度达到该比例后进入静默
    private static let silenceTriggerPercent = 30

    private var rng: SplitMix64
    private var silencedUntil: TimeInterval?

    public init(
        profile: SimDeviceProfile = SimDeviceProfile(),
        files: [SimFile] = [],
        faults: SimFaults = SimFaults(),
        randomSeed: UInt64 = 0xBEEC_0DE1
    ) {
        self.profile = profile
        self.faults = faults
        self.files = files.sorted { $0.id > $1.id }
        self.rng = SplitMix64(seed: randomSeed)
    }

    /// 当前对客户端可见的文件库（应用空列表/异常文件名故障后的视图）
    public var visibleFiles: [SimFile] {
        var result = faults.emptyFileList ? [] : files
        if faults.hostileFileNames {
            result.append(contentsOf: SimFile.hostileEntries())
        }
        return result.sorted { $0.id > $1.id }
    }

    /// 是否处于静默期（故障注入"推流中途断连"）
    public func isSilenced(now: TimeInterval) -> Bool {
        guard let until = silencedUntil else { return false }
        return now < until
    }

    /// 连接断开/重置会话状态：作废未完成推流与静默，统计计数保留
    public mutating func reset() {
        activeStream = nil
        silencedUntil = nil
    }

    /// 订阅后主动推送 INFO（§6.1，session=0x00）
    public mutating func makeInfoPush() -> [SimOutboundFrame] {
        emit(infoFrames(session: 0))
    }

    /// 处理一条完整消息帧，返回待发送帧序列（可为空）。
    /// - Parameters:
    ///   - frame: 已通过帧级校验且完成分片重组的消息帧
    ///   - now: 单调时间（秒），静默判定用
    public mutating func handle(_ frame: BuzzLinkFrame, now: TimeInterval) -> [SimOutboundFrame] {
        receivedFrameCount += 1
        recoverIfSilenceExpired(now: now)
        guard silencedUntil == nil else { return [] }
        guard let type = BuzzLinkFrameType(rawValue: frame.type) else {
            return emit([errorFrame(session: frame.session, code: 4000, message: "unknown frame type")])
        }
        switch type {
        case .ping:
            return emit([SimOutboundFrame(frame: BuzzLinkFrame(
                type: BuzzLinkFrameType.pong.rawValue,
                session: frame.session,
                channel: BuzzLinkChannel.link.rawValue,
                payload: frame.payload
            ))])
        case .infoGet:
            return emit(infoFrames(session: frame.session))
        case .listReq:
            return listResponse(frame)
        case .fileInfoReq:
            return fileInfoResponse(frame)
        case .dlBeginReq:
            return beginDownload(frame)
        case .dlEndReq:
            return endDownload(frame, aborted: false)
        case .dlAbort:
            return endDownload(frame, aborted: true)
        case .fileDeleteReq:
            // §5.1 删除为 P1 可选项，Sim P0 不实现
            return emit([errorFrame(session: frame.session, code: 4003, message: "delete not supported")])
        default:
            return emit([errorFrame(session: frame.session, code: 4000, message: "unexpected frame type")])
        }
    }

    /// 拉取下一块 DL_CHUNK。
    /// - Returns: 待发送块；流结束/空闲/静默中返回 nil
    public mutating func nextChunk(now: TimeInterval) -> SimOutboundFrame? {
        // 静默期满：模拟设备恢复——作废未完成流（客户端此时已超时，§7 走 ABORT + 重新 BEGIN）
        recoverIfSilenceExpired(now: now)
        guard silencedUntil == nil else { return nil }
        guard var stream = activeStream else { return nil }

        // 故障：推流中途静默（达到触发进度后冻结链路）
        if faults.silentMidDownload, !stream.didEnterSilence,
           stream.nextSeq * 100 >= Self.silenceTriggerPercent * stream.totalChunks {
            stream.didEnterSilence = true
            activeStream = stream
            silencedUntil = now + faults.silentDuration
            return nil
        }

        while stream.nextSeq < stream.totalChunks {
            let seq = stream.nextSeq
            stream.nextSeq += 1
            // 故障：随机丢块——不发送但序号照常递增，由文件 CRC 终裁（§7）
            if shouldDropChunk() {
                droppedChunkCount += 1
                continue
            }
            guard let file = file(forID: stream.fileID) else {
                activeStream = nil
                return nil
            }
            let start = seq * stream.chunkDataLen
            let end = min(start + stream.chunkDataLen, file.data.count)
            var payload = Data()
            payload.reserveCapacity(2 + (end - start))
            payload.appendSimLE16(UInt16(seq))
            payload.append(file.data.subdata(in: start..<end))
            stream.sentChunks += 1
            let isFinal = stream.nextSeq == stream.totalChunks
            activeStream = stream
            let frame = BuzzLinkFrame(
                type: BuzzLinkFrameType.dlChunk.rawValue,
                session: stream.session,
                channel: BuzzLinkChannel.file.rawValue,
                payload: payload
            )
            sentFrameCount += 1
            return SimOutboundFrame(frame: frame, corruptCRC: faults.corruptFinalChunkCRC && isFinal)
        }

        // 全部块已决策完毕：推流结束（丢失的块不补发，等待客户端超时/重下）
        activeStream = nil
        return nil
    }

    // MARK: - 控制通道命令

    private mutating func listResponse(_ frame: BuzzLinkFrame) -> [SimOutboundFrame] {
        struct Request: Decodable { let offset: Int; let limit: Int? }
        guard let request = try? JSONDecoder().decode(Request.self, from: frame.payload) else {
            return emit([errorFrame(session: frame.session, code: 4000, message: "bad request")])
        }
        let all = visibleFiles
        let limit = min(request.limit ?? BuzzLinkConstants.listPageLimit, BuzzLinkConstants.listPageLimit)
        guard request.offset >= 0, request.offset <= all.count, limit > 0 else {
            return emitJSONError(type: .listResp, session: frame.session, code: 4000, message: "bad offset")
        }
        let page = Array(all[request.offset..<min(request.offset + limit, all.count)])
        let json = ListResponseJSON(
            status: "ok",
            total: all.count,
            offset: request.offset,
            files: page.map { FileEntryJSON(id: $0.id, durationMs: $0.durationMs, size: $0.data.count) }
        )
        return emitJSON(ListResponseJSON.self, value: json, type: .listResp, session: frame.session)
    }

    private mutating func fileInfoResponse(_ frame: BuzzLinkFrame) -> [SimOutboundFrame] {
        struct Request: Decodable { let id: String? }
        guard let request = try? JSONDecoder().decode(Request.self, from: frame.payload),
              let id = request.id else {
            return emit([errorFrame(session: frame.session, code: 4000, message: "bad request")])
        }
        guard let file = file(forID: id) else {
            return emitJSONError(type: .fileInfoResp, session: frame.session, code: 4004, message: "file not found")
        }
        let json = FileInfoJSON(
            status: "ok",
            id: file.id,
            size: file.data.count,
            crc16: file.crc16,
            durationMs: file.durationMs,
            format: "ogg-opus",
            sampleRate: 16_000,
            channels: 1
        )
        return emitJSON(FileInfoJSON.self, value: json, type: .fileInfoResp, session: frame.session)
    }

    private mutating func infoFrames(session: UInt8) -> [SimOutboundFrame] {
        let effectiveFree = faults.storageNearlyFull ? profile.storageTotal / 20 : profile.storageFree
        let json = InfoJSON(
            model: profile.model,
            fwVersion: profile.fwVersion,
            protocolVersion: Int(BuzzLinkConstants.version),
            deviceName: profile.deviceName,
            battery: profile.battery,
            charging: profile.isCharging,
            storageTotal: profile.storageTotal,
            storageFree: effectiveFree
        )
        return encodeJSON(json, type: .info, session: session)
    }

    // MARK: - 文件通道命令

    private mutating func beginDownload(_ frame: BuzzLinkFrame) -> [SimOutboundFrame] {
        guard let id = Self.parseFileID(frame.payload) else {
            return emit([beginResponseFrame(session: frame.session, status: .badRequest)])
        }
        if activeStream != nil {
            return emit([beginResponseFrame(session: frame.session, status: .busy)])
        }
        guard let file = file(forID: id) else {
            return emit([beginResponseFrame(session: frame.session, status: .notFound)])
        }
        let chunkDataLen = BuzzLinkConstants.defaultChunkDataLength
        let totalChunks = (file.data.count + chunkDataLen - 1) / chunkDataLen
        activeStream = SimStreamState(
            fileID: id,
            session: frame.session,
            chunkDataLen: chunkDataLen,
            totalChunks: totalChunks,
            nextSeq: 0,
            sentChunks: 0,
            didEnterSilence: false
        )
        return emit([beginResponseFrame(
            session: frame.session,
            status: .ok,
            chunkDataLen: UInt16(chunkDataLen),
            totalChunks: UInt32(clamping: totalChunks),
            size: UInt32(clamping: file.data.count),
            fileCRC: file.crc16
        )])
    }

    private mutating func endDownload(_ frame: BuzzLinkFrame, aborted: Bool) -> [SimOutboundFrame] {
        guard let id = Self.parseFileID(frame.payload) else {
            return emit([endResponseFrame(session: frame.session, status: .badRequest, fileCRC: 0)])
        }
        let file = file(forID: id)
        if aborted {
            // §5.1：ABORT 停止推流并回 DL_END_RESP(status=2)
            if activeStream?.fileID == id { activeStream = nil }
            return emit([endResponseFrame(
                session: frame.session,
                status: file == nil ? .notFound : .aborted,
                fileCRC: file?.crc16 ?? 0
            )])
        }
        guard let file else {
            return emit([endResponseFrame(session: frame.session, status: .notFound, fileCRC: 0)])
        }
        if let stream = activeStream, stream.fileID != id {
            return emit([endResponseFrame(session: frame.session, status: .badRequest, fileCRC: 0)])
        }
        activeStream = nil
        return emit([endResponseFrame(session: frame.session, status: .ok, fileCRC: file.crc16)])
    }

    // MARK: - 私有：组帧与编码

    private func file(forID id: String) -> SimFile? {
        visibleFiles.first { $0.id == id }
    }

    private mutating func shouldDropChunk() -> Bool {
        faults.dropChunkProbability > 0 && rng.nextProbability() < faults.dropChunkProbability
    }

    /// 静默期满后的懒恢复：作废未完成流（模拟设备重启上线）
    private mutating func recoverIfSilenceExpired(now: TimeInterval) {
        if let until = silencedUntil, now >= until {
            silencedUntil = nil
            activeStream = nil
        }
    }

    private mutating func emit(_ frames: [SimOutboundFrame]) -> [SimOutboundFrame] {
        sentFrameCount += frames.count
        return frames
    }

    private mutating func emitJSON<T: Encodable>(
        _: T.Type,
        value: T,
        type: BuzzLinkFrameType,
        session: UInt8
    ) -> [SimOutboundFrame] {
        emit(encodeJSON(value, type: type, session: session))
    }

    private mutating func emitJSONError(
        type: BuzzLinkFrameType,
        session: UInt8,
        code: Int,
        message: String
    ) -> [SimOutboundFrame] {
        emit(encodeJSON(ErrorJSON(status: "error", code: code, message: message), type: type, session: session))
    }

    /// JSON 编码 + 按 §3.3 分片；编码失败降级为 ERROR 帧（不会发生，防御）
    private func encodeJSON<T: Encodable>(
        _ value: T,
        type: BuzzLinkFrameType,
        session: UInt8
    ) -> [SimOutboundFrame] {
        do {
            let data = try JSONEncoder().encode(value)
            let frames = try BuzzLinkFragmenter.fragment(
                type: type.rawValue,
                session: session,
                channel: BuzzLinkChannel.control.rawValue,
                message: data
            )
            return frames.map { SimOutboundFrame(frame: $0) }
        } catch {
            return [errorFrame(session: session, code: 4999, message: "encode failure")]
        }
    }

    private func errorFrame(session: UInt8, code: Int, message: String) -> SimOutboundFrame {
        let frames = encodeJSON(ErrorJSON(status: "error", code: code, message: message), type: .error, session: session)
        return frames.first ?? SimOutboundFrame(frame: BuzzLinkFrame(
            type: BuzzLinkFrameType.error.rawValue,
            session: session,
            channel: BuzzLinkChannel.control.rawValue
        ))
    }

    /// DL_BEGIN_REQ/DL_END_REQ/DL_ABORT 的 payload：`[idLen u8][id…]`（§5.1）
    private static func parseFileID(_ payload: Data) -> String? {
        guard let length = payload.first else { return nil }
        let lengthInt = Int(length)
        guard lengthInt >= 1, lengthInt <= BuzzLinkConstants.maxFileIDLength,
              payload.count == 1 + lengthInt else {
            return nil
        }
        return String(data: payload.dropFirst(), encoding: .utf8)
    }

    /// DL_BEGIN_RESP 13 字节定长 payload（§5.1）
    private func beginResponseFrame(
        session: UInt8,
        status: BuzzLinkStatus,
        chunkDataLen: UInt16 = 0,
        totalChunks: UInt32 = 0,
        size: UInt32 = 0,
        fileCRC: UInt16 = 0
    ) -> SimOutboundFrame {
        var payload = Data()
        payload.reserveCapacity(13)
        payload.append(status.rawValue)
        payload.appendSimLE16(chunkDataLen)
        payload.appendSimLE32(totalChunks)
        payload.appendSimLE32(size)
        payload.appendSimLE16(fileCRC)
        return SimOutboundFrame(frame: BuzzLinkFrame(
            type: BuzzLinkFrameType.dlBeginResp.rawValue,
            session: session,
            channel: BuzzLinkChannel.file.rawValue,
            payload: payload
        ))
    }

    /// DL_END_RESP 3 字节定长 payload（§5.1）
    private func endResponseFrame(session: UInt8, status: BuzzLinkStatus, fileCRC: UInt16) -> SimOutboundFrame {
        var payload = Data()
        payload.append(status.rawValue)
        payload.appendSimLE16(fileCRC)
        return SimOutboundFrame(frame: BuzzLinkFrame(
            type: BuzzLinkFrameType.dlEndResp.rawValue,
            session: session,
            channel: BuzzLinkChannel.file.rawValue,
            payload: payload
        ))
    }
}

// MARK: - JSON 数据模型（BUZZLINK.md §8）

private struct InfoJSON: Encodable {
    let model: String
    let fwVersion: String
    let protocolVersion: Int
    let deviceName: String
    let battery: Int
    let charging: Bool
    let storageTotal: UInt64
    let storageFree: UInt64

    enum CodingKeys: String, CodingKey {
        case model, fwVersion, deviceName, battery, charging, storageTotal, storageFree
        case protocolVersion = "protocol"
    }
}

private struct FileEntryJSON: Encodable {
    let id: String
    let durationMs: UInt32
    let size: Int
}

private struct ListResponseJSON: Encodable {
    let status: String
    let total: Int
    let offset: Int
    let files: [FileEntryJSON]
}

private struct FileInfoJSON: Encodable {
    let status: String
    let id: String
    let size: Int
    let crc16: UInt16
    let durationMs: UInt32
    let format: String
    let sampleRate: Int
    let channels: Int
}

private struct ErrorJSON: Encodable {
    let status: String
    let code: Int
    let message: String
}

// MARK: - 小端拼装（本模块私有；BuzzLinkCore 的 Data+LE 为 module 内部）

private extension Data {
    mutating func appendSimLE16(_ value: UInt16) {
        append(UInt8(value & 0xFF))
        append(UInt8(value >> 8))
    }

    mutating func appendSimLE32(_ value: UInt32) {
        append(UInt8(value & 0xFF))
        append(UInt8((value >> 8) & 0xFF))
        append(UInt8((value >> 16) & 0xFF))
        append(UInt8(value >> 24))
    }
}
