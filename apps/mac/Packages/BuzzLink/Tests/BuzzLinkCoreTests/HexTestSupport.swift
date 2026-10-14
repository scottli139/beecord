import Foundation

/// 测试工具：hex 字符串（容忍空格）→ Data；非法字符静默跳过
func dataFromHex(_ hex: String) -> Data {
    let stripped = hex.filter { !$0.isWhitespace }
    var data = Data()
    var index = stripped.startIndex
    while index < stripped.endIndex {
        let next = stripped.index(index, offsetBy: 2, limitedBy: stripped.endIndex) ?? stripped.endIndex
        if let byte = UInt8(stripped[index..<next], radix: 16) {
            data.append(byte)
        }
        index = next
    }
    return data
}
