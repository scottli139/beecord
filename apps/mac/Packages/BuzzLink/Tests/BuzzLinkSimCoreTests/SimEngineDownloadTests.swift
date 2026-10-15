import BuzzLinkCore
import BuzzLinkSimCore
import XCTest

/// 文件下载会话与故障注入（BUZZLINK.md §6/§7；TESTING.md §5）。
final class SimEngineDownloadTests: XCTestCase {

    private let fileID = "REC_20260101_120000"

    private func makeEngine(
        fileSize: Int = 772,
        faults: SimFaults = SimFaults(),
        seed: UInt64 = 42
    ) -> SimEngine {
        SimEngine(
            files: [SimTestSupport.makeFile(id: fileID, durationMs: 1_000, size: fileSize, seed: 1)],
            faults: faults,
            randomSeed: seed
        )
    }

    /// 发起下载并取出 DL_BEGIN_RESP 消息
    private func beginDownload(
        engine: inout SimEngine,
        client: ClientHarness,
        id: String? = nil,
        session: UInt8 = 0x31
    ) throws -> [String: Int] {
        let messages = try client.receive(engine.handle(
            SimTestSupport.fileRequest(
                type: .dlBeginReq,
                session: session,
                payload: SimTestSupport.idPayload(id ?? fileID)
            ),
            now: 0
        ))
        let response = try XCTUnwrap(messages.first)
        XCTAssertEqual(response.type, BuzzLinkFrameType.dlBeginResp.rawValue)
        XCTAssertEqual(response.payload.count, 13)
        return [
            "status": Int(response.payload[response.payload.startIndex]),
            "chunkDataLen": Int(response.payload.readLE16ForTest(at: 1)),
            "totalChunks": Int(response.payload.readLE32ForTest(at: 3)),
            "size": Int(response.payload.readLE32ForTest(at: 7)),
            "fileCrc16": Int(response.payload.readLE16ForTest(at: 11))
        ]
    }

    /// 拉取推流直到结束，返回客户端收到的 DL_CHUNK 消息
    private func pullAllChunks(
        engine: inout SimEngine,
        client: ClientHarness,
        now: TimeInterval = 0
    ) throws -> [BuzzLinkFrame] {
        var chunks: [BuzzLinkFrame] = []
        while let outbound = engine.nextChunk(now: now) {
            chunks.append(contentsOf: try client.receive(outbound))
        }
        return chunks
    }

    // MARK: - DL_BEGIN

    func testBeginDownloadResponseFields() throws {
        var engine = makeEngine(fileSize: 772)
        let client = ClientHarness()
        let fields = try beginDownload(engine: &engine, client: client)
        XCTAssertEqual(fields["status"], 0)
        XCTAssertEqual(fields["chunkDataLen"], BuzzLinkConstants.defaultChunkDataLength)
        XCTAssertEqual(fields["totalChunks"], 4) // ceil(772/224)
        XCTAssertEqual(fields["size"], 772)
        let file = SimTestSupport.makeFile(id: fileID, size: 772, seed: 1)
        XCTAssertEqual(fields["fileCrc16"], Int(file.crc16))
    }

    func testBeginDownloadUnknownID() throws {
        var engine = makeEngine()
        let client = ClientHarness()
        let fields = try beginDownload(engine: &engine, client: client, id: "REC_29990101_000000")
        XCTAssertEqual(fields["status"], BuzzLinkStatus.notFound.rawValue.int)
        XCTAssertNil(engine.activeStream)
    }

    func testBeginDownloadMalformedPayload() throws {
        var engine = makeEngine()
        let client = ClientHarness()
        // idLen 与实际长度不符
        let messages = try client.receive(engine.handle(
            SimTestSupport.fileRequest(type: .dlBeginReq, session: 1, payload: Data([0x10, 0x41, 0x42])),
            now: 0
        ))
        XCTAssertEqual(messages.first?.type, BuzzLinkFrameType.dlBeginResp.rawValue)
        XCTAssertEqual(messages.first?.payload[messages.first?.payload.startIndex ?? 0], BuzzLinkStatus.badRequest.rawValue)
        XCTAssertNil(engine.activeStream)
    }

    func testBeginDownloadWhileStreamingIsBusy() throws {
        var engine = makeEngine(fileSize: 4_480) // 20 块，推不完
        let client = ClientHarness()
        _ = try beginDownload(engine: &engine, client: client)
        _ = try client.receive(engine.nextChunk(now: 0)) // 推 1 块，流仍进行中
        let fields = try beginDownload(engine: &engine, client: client, session: 0x32)
        XCTAssertEqual(fields["status"], BuzzLinkStatus.busy.rawValue.int)
        // 原流不受影响
        XCTAssertEqual(engine.activeStream?.session, 0x31)
    }

    // MARK: - DL_CHUNK / 完整下载

    func testFullDownloadRoundTrip() throws {
        var engine = makeEngine(fileSize: 772)
        let client = ClientHarness()
        let fields = try beginDownload(engine: &engine, client: client)
        let totalChunks = try XCTUnwrap(fields["totalChunks"])

        let chunks = try pullAllChunks(engine: &engine, client: client)
        XCTAssertEqual(chunks.count, totalChunks)
        XCTAssertNil(engine.activeStream)

        // 逐块校验：type/session/channel、seq 连续、除末块外每块恰 chunkDataLen（§6.2）
        var reassembled = Data()
        for (index, chunk) in chunks.enumerated() {
            XCTAssertEqual(chunk.type, BuzzLinkFrameType.dlChunk.rawValue)
            XCTAssertEqual(chunk.session, 0x31)
            XCTAssertEqual(chunk.channel, BuzzLinkChannel.file.rawValue)
            XCTAssertEqual(Int(chunk.payload.readLE16ForTest(at: 0)), index)
            let data = chunk.payload.dropFirst(2)
            if index < totalChunks - 1 {
                XCTAssertEqual(data.count, BuzzLinkConstants.defaultChunkDataLength)
            } else {
                XCTAssertEqual(data.count, 772 - 3 * BuzzLinkConstants.defaultChunkDataLength) // 100
            }
            reassembled.append(data)
        }

        // 三处 CRC 全等（§6.3）：本地重组 / BEGIN 携带 / END 返回
        let file = SimTestSupport.makeFile(id: fileID, size: 772, seed: 1)
        XCTAssertEqual(reassembled, file.data)
        XCTAssertEqual(fields["fileCrc16"], Int(CRC16.ccittFalse(reassembled)))

        let endMessages = try client.receive(engine.handle(
            SimTestSupport.fileRequest(type: .dlEndReq, session: 0x31, payload: SimTestSupport.idPayload(fileID)),
            now: 0
        ))
        let endResponse = try XCTUnwrap(endMessages.first)
        XCTAssertEqual(endResponse.type, BuzzLinkFrameType.dlEndResp.rawValue)
        XCTAssertEqual(endResponse.payload.count, 3)
        XCTAssertEqual(endResponse.payload[endResponse.payload.startIndex], BuzzLinkStatus.ok.rawValue)
        XCTAssertEqual(Int(endResponse.payload.readLE16ForTest(at: 1)), Int(file.crc16))
    }

    func testChunkSizesExactMultiple() throws {
        // size 恰为 chunkDataLen 整数倍：末块也是整块
        var engine = makeEngine(fileSize: 3 * BuzzLinkConstants.defaultChunkDataLength)
        let client = ClientHarness()
        let fields = try beginDownload(engine: &engine, client: client)
        XCTAssertEqual(fields["totalChunks"], 3)
        let chunks = try pullAllChunks(engine: &engine, client: client)
        for chunk in chunks {
            XCTAssertEqual(chunk.payload.count, 2 + BuzzLinkConstants.defaultChunkDataLength)
        }
    }

    // MARK: - DL_END / DL_ABORT

    func testEndDownloadWithMismatchedID() throws {
        var engine = makeEngine(fileSize: 4_480)
        let client = ClientHarness()
        _ = try beginDownload(engine: &engine, client: client)
        let messages = try client.receive(engine.handle(
            SimTestSupport.fileRequest(type: .dlEndReq, session: 0x31, payload: SimTestSupport.idPayload("REC_20000101_000000")),
            now: 0
        ))
        // id 不在库 → not_found；且不影响进行中的流
        XCTAssertEqual(messages.first?.payload[messages.first?.payload.startIndex ?? 0], BuzzLinkStatus.notFound.rawValue)
        XCTAssertNotNil(engine.activeStream)
    }

    func testEndDownloadWithOtherStreamingFileID() throws {
        var engine = SimEngine(
            files: [
                SimTestSupport.makeFile(id: "REC_20260101_120000", size: 4_480, seed: 1),
                SimTestSupport.makeFile(id: "REC_20260102_120000", size: 100, seed: 2)
            ],
            randomSeed: 42
        )
        let client = ClientHarness()
        _ = try beginDownload(engine: &engine, client: client)
        // 流式进行中 END 另一个"存在但未在下载"的文件 → bad_request，原流不动
        let messages = try client.receive(engine.handle(
            SimTestSupport.fileRequest(type: .dlEndReq, session: 0x31, payload: SimTestSupport.idPayload("REC_20260102_120000")),
            now: 0
        ))
        XCTAssertEqual(messages.first?.payload[messages.first?.payload.startIndex ?? 0], BuzzLinkStatus.badRequest.rawValue)
        XCTAssertEqual(engine.activeStream?.fileID, fileID)
    }

    func testAbortStopsStream() throws {
        var engine = makeEngine(fileSize: 4_480)
        let client = ClientHarness()
        _ = try beginDownload(engine: &engine, client: client)
        _ = try client.receive(engine.nextChunk(now: 0))

        let messages = try client.receive(engine.handle(
            SimTestSupport.fileRequest(type: .dlAbort, session: 0x31, payload: SimTestSupport.idPayload(fileID)),
            now: 0
        ))
        // §5.1：ABORT 应回 DL_END_RESP(status=2)
        let response = try XCTUnwrap(messages.first)
        XCTAssertEqual(response.type, BuzzLinkFrameType.dlEndResp.rawValue)
        XCTAssertEqual(response.payload[response.payload.startIndex], BuzzLinkStatus.aborted.rawValue)
        let file = SimTestSupport.makeFile(id: fileID, size: 4_480, seed: 1)
        XCTAssertEqual(Int(response.payload.readLE16ForTest(at: 1)), Int(file.crc16))
        XCTAssertNil(engine.activeStream)
        XCTAssertNil(engine.nextChunk(now: 0))
    }

    // MARK: - 故障注入（TESTING.md §5）

    func testDropChunksFault() throws {
        var faults = SimFaults()
        faults.dropChunkProbability = 0.5
        var engine = makeEngine(fileSize: 22_400, faults: faults) // 100 块
        let client = ClientHarness()
        let fields = try beginDownload(engine: &engine, client: client)
        let totalChunks = try XCTUnwrap(fields["totalChunks"])
        XCTAssertEqual(totalChunks, 100)

        let chunks = try pullAllChunks(engine: &engine, client: client)
        XCTAssertGreaterThan(engine.droppedChunkCount, 0)
        XCTAssertEqual(chunks.count + engine.droppedChunkCount, totalChunks)

        // 客户端视角：seq 单调但有洞（§7：跳号记录并继续，最终文件 CRC 裁决）
        let seqs = chunks.map { Int($0.payload.readLE16ForTest(at: 0)) }
        XCTAssertEqual(seqs, seqs.sorted())
        XCTAssertEqual(Set(seqs).count, seqs.count)
        XCTAssertLessThan(Set(seqs).count, totalChunks)
    }

    func testDropAllChunksStillTerminates() throws {
        var faults = SimFaults()
        faults.dropChunkProbability = 1.0
        var engine = makeEngine(fileSize: 2_240, faults: faults)
        let client = ClientHarness()
        _ = try beginDownload(engine: &engine, client: client)
        let chunks = try pullAllChunks(engine: &engine, client: client)
        XCTAssertEqual(chunks.count, 0)
        XCTAssertEqual(engine.droppedChunkCount, 10)
        XCTAssertNil(engine.activeStream) // 推流循环必须能退出，不得死循环
    }

    func testCorruptFinalChunkCRC() throws {
        var faults = SimFaults()
        faults.corruptFinalChunkCRC = true
        var engine = makeEngine(fileSize: 772, faults: faults)
        let begin = engine.handle(
            SimTestSupport.fileRequest(type: .dlBeginReq, session: 1, payload: SimTestSupport.idPayload(fileID)),
            now: 0
        )
        XCTAssertFalse(try XCTUnwrap(begin.first).corruptCRC)

        var outbound: [SimOutboundFrame] = []
        while let chunk = engine.nextChunk(now: 0) {
            outbound.append(chunk)
        }
        XCTAssertEqual(outbound.count, 4)
        // 仅末块带损坏标记（TESTING.md §5"末帧 CRC 错误"）
        for chunk in outbound.dropLast() {
            XCTAssertFalse(chunk.corruptCRC)
            XCTAssertNoThrow(try BuzzLinkFrame.decoded(from: SimTestSupport.wireBytes(chunk)))
        }
        let final = try XCTUnwrap(outbound.last)
        XCTAssertTrue(final.corruptCRC)
        // 线上字节过不了帧 CRC：客户端 parser 丢弃并计数（BUZZLINK.md §3.3 重同步）
        let corrupted = try SimTestSupport.wireBytes(final)
        XCTAssertThrowsError(try BuzzLinkFrame.decoded(from: corrupted)) { error in
            guard case BuzzLinkFrameError.crcMismatch = error else {
                return XCTFail("应为 crcMismatch，实际 \(error)")
            }
        }
        var parser = BuzzLinkFrameParser()
        parser.append(corrupted)
        XCTAssertNil(parser.nextFrame())
        XCTAssertGreaterThan(parser.discardedFrameCount, 0)
    }

    func testSilentMidDownload() throws {
        var faults = SimFaults()
        faults.silentMidDownload = true
        faults.silentDuration = 12
        var engine = makeEngine(fileSize: 2_240, faults: faults) // 10 块，30% 处触发
        let client = ClientHarness()
        _ = try beginDownload(engine: &engine, client: client)

        // 触发点 30%：seq 0、1、2 正常发出（3 块后进度 30%）
        for _ in 0..<3 {
            XCTAssertNotNil(engine.nextChunk(now: 0))
        }
        XCTAssertNil(engine.nextChunk(now: 0))
        XCTAssertTrue(engine.isSilenced(now: 0))
        XCTAssertNotNil(engine.activeStream) // 流未作废，只是冻结

        // 静默期：一切命令无响应（模拟链路冻结）
        let ping = BuzzLinkFrame(type: BuzzLinkFrameType.ping.rawValue, session: 1, channel: BuzzLinkChannel.link.rawValue)
        XCTAssertTrue(engine.handle(ping, now: 5).isEmpty)
        XCTAssertTrue(engine.handle(ping, now: 11.9).isEmpty)
        XCTAssertTrue(engine.isSilenced(now: 11.9))

        // 静默期满：懒恢复——流作废（客户端已超时，§7 重新 BEGIN）
        XCTAssertNil(engine.nextChunk(now: 12.0))
        XCTAssertFalse(engine.isSilenced(now: 12.0))
        XCTAssertNil(engine.activeStream)

        // 恢复后命令正常，可重新下载
        let messages = try client.receive(engine.handle(ping, now: 12.1))
        XCTAssertEqual(messages.first?.type, BuzzLinkFrameType.pong.rawValue)
        let fields = try beginDownload(engine: &engine, client: client, session: 0x33)
        XCTAssertEqual(fields["status"], 0)
    }

    func testAbortDuringSilenceIsDroppedThenRecoverable() throws {
        var faults = SimFaults()
        faults.silentMidDownload = true
        faults.silentDuration = 12
        var engine = makeEngine(fileSize: 2_240, faults: faults)
        let client = ClientHarness()
        _ = try beginDownload(engine: &engine, client: client)
        _ = engine.nextChunk(now: 0)
        _ = engine.nextChunk(now: 0)
        _ = engine.nextChunk(now: 0)
        XCTAssertNil(engine.nextChunk(now: 0)) // 进入静默

        // 客户端超时后发 ABORT（§7）：静默期被丢弃
        XCTAssertTrue(engine.handle(
            SimTestSupport.fileRequest(type: .dlAbort, session: 0x31, payload: SimTestSupport.idPayload(fileID)),
            now: 11
        ).isEmpty)

        // 静默期满后重发 ABORT：清理成功，回 status=2
        let messages = try client.receive(engine.handle(
            SimTestSupport.fileRequest(type: .dlAbort, session: 0x31, payload: SimTestSupport.idPayload(fileID)),
            now: 12.5
        ))
        XCTAssertEqual(messages.first?.payload[messages.first?.payload.startIndex ?? 0], BuzzLinkStatus.aborted.rawValue)
        XCTAssertNil(engine.activeStream)
    }

    // MARK: - 会话与重置

    func testResetClearsStreamState() throws {
        var engine = makeEngine(fileSize: 4_480)
        let client = ClientHarness()
        _ = try beginDownload(engine: &engine, client: client)
        engine.reset() // 模拟连接断开
        XCTAssertNil(engine.activeStream)
        XCTAssertNil(engine.nextChunk(now: 0))
        // 重连后可重新 BEGIN（幂等，§7）
        let fields = try beginDownload(engine: &engine, client: client, session: 0x40)
        XCTAssertEqual(fields["status"], 0)
    }

    func testSplitMix64Deterministic() {
        var a = SplitMix64(seed: 99)
        var b = SplitMix64(seed: 99)
        for _ in 0..<100 {
            XCTAssertEqual(a.next(), b.next())
        }
        var c = SplitMix64(seed: 100)
        var d = SplitMix64(seed: 99)
        XCTAssertNotEqual(c.next(), d.next())
    }

    func testHostileEntriesWithinProtocolLimits() {
        for entry in SimFile.hostileEntries() {
            XCTAssertLessThanOrEqual(entry.id.utf8.count, BuzzLinkConstants.maxFileIDLength)
            XCTAssertFalse(entry.data.isEmpty)
        }
    }
}

private extension UInt8 {
    var int: Int { Int(self) }
}

private extension Data {
    func readLE16ForTest(at offset: Int) -> UInt16 {
        UInt16(self[index(startIndex, offsetBy: offset)]) |
            (UInt16(self[index(startIndex, offsetBy: offset + 1)]) << 8)
    }

    func readLE32ForTest(at offset: Int) -> UInt32 {
        UInt32(self[index(startIndex, offsetBy: offset)]) |
            (UInt32(self[index(startIndex, offsetBy: offset + 1)]) << 8) |
            (UInt32(self[index(startIndex, offsetBy: offset + 2)]) << 16) |
            (UInt32(self[index(startIndex, offsetBy: offset + 3)]) << 24)
    }
}
