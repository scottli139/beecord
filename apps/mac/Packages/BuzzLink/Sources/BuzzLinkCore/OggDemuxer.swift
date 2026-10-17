import Foundation

/// Ogg 页解封装（纯 Swift，无 AVFoundation 依赖；studio-tech-design §5.1 spike 结论 3 的上游）。
///
/// 只做"读遍历"不做容错恢复：结构异常抛 ``OggDemuxError`` 具名错（T6 转码 failed 态呈现），
/// 绝不返回半成品静默成功。校验项：捕获模式 "OggS"、流版本 0、序号连续性（按 serial）、
/// 页 CRC32（Ogg 多项式 0x04C11DB7，初始值 0，不反射）。
public enum OggDemuxer {

    /// 一页 Ogg 数据（segments 保留分包边界：长度 <255 的段结束一个分包）。
    public struct Page: Sendable, Equatable {
        /// 头类型标志位：0x01 续页（首段续前页未完成的分包）、0x02 BOS、0x04 EOS
        public var headerType: UInt8
        /// 颗粒位置（解码时间基准；本解封装不解释）
        public var granulePosition: UInt64
        /// 比特流序列号（多路复用区分；Opus 文件恒单流）
        public var serial: UInt32
        /// 页序号（同 serial 内单调递增，从 0 起）
        public var sequence: UInt32
        /// 段表（每段 0–255 字节；<255 表示一个分包在此结束）
        public var segments: [Data]
        /// 页数据段拼接视图
        public var payload: Data {
            var data = Data()
            for segment in segments { data.append(segment) }
            return data
        }
        /// 是否 BOS 页
        public var isBeginningOfStream: Bool { headerType & 0x02 != 0 }
        /// 是否 EOS 页
        public var isEndOfStream: Bool { headerType & 0x04 != 0 }
        /// 是否续页（首段续前一页未完成的分包）
        public var isContinued: Bool { headerType & 0x01 != 0 }
    }

    /// 解封装错误（具名；UI 层映射"转码失败"行态）
    public enum OggDemuxError: Error, LocalizedError, Sendable, Equatable {
        case truncatedCapture
        case badCapturePattern
        case unsupportedVersion(UInt8)
        case truncatedPage(bytesAvailable: Int)
        case sequenceGap(serial: UInt32, expected: UInt32, actual: UInt32)
        case pageChecksumMismatch(sequence: UInt32)
        case missingBeginningOfStream
        case missingEndOfStream
        case orphanContinuationPage(sequence: UInt32)

        public var errorDescription: String? {
            switch self {
            case .truncatedCapture:
                "Ogg 文件截断：捕获模式不完整"
            case .badCapturePattern:
                "Ogg 捕获模式错误（非 \"OggS\"）"
            case .unsupportedVersion(let version):
                "Ogg 流版本不支持：\(version)"
            case .truncatedPage(let available):
                "Ogg 页截断（头/段表/数据不完整，剩余 \(available) 字节）"
            case .sequenceGap(let serial, let expected, let actual):
                "Ogg 页序号断裂：serial=\(serial) 期望 \(expected) 实际 \(actual)"
            case .pageChecksumMismatch(let sequence):
                "Ogg 页 CRC 校验失败：seq=\(sequence)"
            case .missingBeginningOfStream:
                "Ogg 流缺少 BOS 起始页"
            case .missingEndOfStream:
                "Ogg 流缺少 EOS 结束页（文件不完整）"
            case .orphanContinuationPage(let sequence):
                "Ogg 续页无前置未完成分包：seq=\(sequence)"
            }
        }
    }

    /// 遍历全部 Ogg 页（结构 + 序号 + CRC 校验；首个畸形页即中止）。
    public static func pages(data: Data) throws -> [Page] {
        var result: [Page] = []
        var offset = 0
        var sawBOS = false
        var sawEOS = false
        var lastSequenceBySerial: [UInt32: UInt32] = [:]

        while offset < data.count {
            let remaining = data.count - offset
            guard remaining >= 4 else { throw OggDemuxError.truncatedCapture }
            guard data[offset] == 0x4F,      // 'O'
                  data[offset + 1] == 0x67,  // 'g'
                  data[offset + 2] == 0x67,  // 'g'
                  data[offset + 3] == 0x53   // 'S'
            else { throw OggDemuxError.badCapturePattern }

            guard remaining >= 27 else { throw OggDemuxError.truncatedPage(bytesAvailable: remaining) }
            let version = data[offset + 4]
            guard version == 0 else { throw OggDemuxError.unsupportedVersion(version) }
            let headerType = data[offset + 5]
            let granule = readUInt64(data, offset + 6)
            let serial = readUInt32(data, offset + 14)
            let sequence = readUInt32(data, offset + 18)
            let segmentCount = Int(data[offset + 26])

            let segmentTableStart = offset + 27
            guard remaining >= 27 + segmentCount else {
                throw OggDemuxError.truncatedPage(bytesAvailable: remaining)
            }
            var segmentLengths: [Int] = []
            segmentLengths.reserveCapacity(segmentCount)
            var payloadLength = 0
            for index in 0..<segmentCount {
                let length = Int(data[segmentTableStart + index])
                segmentLengths.append(length)
                payloadLength += length
            }
            let payloadStart = segmentTableStart + segmentCount
            guard data.count - payloadStart >= payloadLength else {
                throw OggDemuxError.truncatedPage(bytesAvailable: data.count - offset)
            }

            try verifyChecksum(data: data, pageStart: offset, pageEnd: payloadStart + payloadLength, sequence: sequence)

            if let last = lastSequenceBySerial[serial], sequence != last + 1 {
                throw OggDemuxError.sequenceGap(serial: serial, expected: last + 1, actual: sequence)
            }
            lastSequenceBySerial[serial] = sequence

            var segments: [Data] = []
            segments.reserveCapacity(segmentCount)
            var cursor = payloadStart
            for length in segmentLengths {
                segments.append(data.subdata(in: cursor..<(cursor + length)))
                cursor += length
            }
            result.append(Page(
                headerType: headerType,
                granulePosition: granule,
                serial: serial,
                sequence: sequence,
                segments: segments
            ))

            if headerType & 0x02 != 0 { sawBOS = true }
            if headerType & 0x04 != 0 { sawEOS = true }
            offset = payloadStart + payloadLength
        }

        guard sawBOS else { throw OggDemuxError.missingBeginningOfStream }
        guard sawEOS else { throw OggDemuxError.missingEndOfStream }
        return result
    }

    /// 抽取 Opus 分包流：跨页重组（续页首段续前一页未完成的分包）。
    /// BOS 页（ID 头）整页按一个分包输出；分包以"段长 <255"收尾（恰 255 的分包靠后续 0 长段收尾）。
    public static func extractOpusPackets(data: Data) throws -> [Data] {
        let allPages = try pages(data: data)
        guard let firstBOS = allPages.first(where: { $0.isBeginningOfStream }) else {
            throw OggDemuxError.missingBeginningOfStream
        }
        // Opus 单流：忽略其它 serial 的页（防御多路复用文件）
        let streamPages = allPages.filter { $0.serial == firstBOS.serial }

        var packets: [Data] = []
        var pending = Data() // 未完成分包累积
        var hasPending = false

        for page in streamPages {
            var segmentIndex = 0
            if page.isContinued {
                guard hasPending else { throw OggDemuxError.orphanContinuationPage(sequence: page.sequence) }
            } else if page.isBeginningOfStream {
                guard !hasPending else { throw OggDemuxError.orphanContinuationPage(sequence: page.sequence) }
                // ID 头页：整页即 Opus ID 分包（单段）
                if let first = page.segments.first, page.segments.count == 1 {
                    packets.append(first)
                } else {
                    packets.append(page.payload)
                }
                continue
            } else if hasPending {
                // 前页分包未完成（末段 255），次页却未声明续页：序列表畸形
                throw OggDemuxError.orphanContinuationPage(sequence: page.sequence)
            }
            while segmentIndex < page.segments.count {
                let segment = page.segments[segmentIndex]
                if page.isContinued && segmentIndex == 0 && hasPending {
                    pending.append(segment)
                } else {
                    pending = segment
                }
                hasPending = true
                segmentIndex += 1
                if segment.count < 255 {
                    packets.append(pending)
                    pending = Data()
                    hasPending = false
                }
            }
        }
        guard !hasPending else { throw OggDemuxError.missingEndOfStream }
        return packets
    }

    // MARK: - 私有

    private static func readUInt32(_ data: Data, _ offset: Int) -> UInt32 {
        var value: UInt32 = 0
        for index in 0..<4 {
            value |= UInt32(data[offset + index]) << (8 * index)
        }
        return value
    }

    private static func readUInt64(_ data: Data, _ offset: Int) -> UInt64 {
        var value: UInt64 = 0
        for index in 0..<8 {
            value |= UInt64(data[offset + index]) << (8 * index)
        }
        return value
    }

    /// Ogg CRC32：多项式 0x04C11DB7，初始值 0，输入输出均不反射；
    /// 校验前将页内 CRC 字段（偏移 22–25）置零参与计算。
    private static func verifyChecksum(data: Data, pageStart: Int, pageEnd: Int, sequence: UInt32) throws {
        var crc: UInt32 = 0
        for position in pageStart..<pageEnd {
            var byte = data[position]
            if position >= pageStart + 22 && position <= pageStart + 25 {
                byte = 0
            }
            crc = (crc << 8) ^ crcTable[Int(UInt8((crc >> 24) ^ UInt32(byte)))]
        }
        let stored = readUInt32(data, pageStart + 22)
        guard crc == stored else {
            throw OggDemuxError.pageChecksumMismatch(sequence: sequence)
        }
    }

    private static let crcTable: [UInt32] = {
        (0..<256).map { index in
            var crc = UInt32(index) << 24
            for _ in 0..<8 {
                crc = (crc & 0x8000_0000) != 0 ? (crc << 1) ^ 0x04C1_1DB7 : crc << 1
            }
            return crc
        }
    }()
}
