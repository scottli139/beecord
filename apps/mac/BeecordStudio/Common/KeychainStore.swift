import Foundation
import Security

/// 最小 Keychain 封装：service/account 定位的通用密码项。
/// 访问属性 kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly（零三方依赖）。
enum KeychainStore {

    enum KeychainError: Error, LocalizedError {
        case unexpectedStatus(OSStatus)

        var errorDescription: String? {
            switch self {
            case .unexpectedStatus(let status):
                "Keychain 操作失败（status=\(status)）"
            }
        }
    }

    /// 读取条目；不存在返回 nil。
    static func read(service: String, account: String) throws -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        if status == errSecItemNotFound {
            return nil
        }
        guard status == errSecSuccess else {
            throw KeychainError.unexpectedStatus(status)
        }
        guard let data = item as? Data, let value = String(data: data, encoding: .utf8) else {
            throw KeychainError.unexpectedStatus(errSecInvalidItemRef)
        }
        return value
    }

    /// 写入条目；`value` 为 nil 或空串时删除条目。
    static func write(_ value: String?, service: String, account: String) throws {
        let baseQuery: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        let attributes: [String: Any] = [
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
        ]
        if let value, !value.isEmpty {
            let payload: [String: Any] = [
                kSecValueData as String: Data(value.utf8),
            ]
            let existing = try read(service: service, account: account)
            let status: OSStatus
            if existing != nil {
                status = SecItemUpdate(baseQuery as CFDictionary, (attributes.merging(payload) { _, new in new }) as CFDictionary)
            } else {
                status = SecItemAdd((baseQuery.merging(attributes) { _, new in new }.merging(payload) { _, new in new }) as CFDictionary, nil)
            }
            guard status == errSecSuccess else {
                throw KeychainError.unexpectedStatus(status)
            }
        } else {
            let status = SecItemDelete(baseQuery as CFDictionary)
            if status != errSecSuccess && status != errSecItemNotFound {
                throw KeychainError.unexpectedStatus(status)
            }
        }
    }
}
