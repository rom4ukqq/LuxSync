import Foundation
import Security

enum Pairing {
    private static let query: [String: Any] = [
        kSecClass as String: kSecClassGenericPassword,
        kSecAttrService as String: "com.brightsync.sender",
        kSecAttrAccount as String: "paired-windows"
    ]

    static func load() -> Data? {
        var found: CFTypeRef?
        var request = query
        request[kSecReturnData as String] = true
        guard SecItemCopyMatching(request as CFDictionary, &found) == errSecSuccess,
              let data = found as? Data, data.count == 32 else { return nil }
        return data
    }

    static func save(_ secret: Data) -> OSStatus {
        let attrs = [kSecValueData as String: secret]
        let status = SecItemUpdate(query as CFDictionary, attrs as CFDictionary)
        if status != errSecItemNotFound { return status }
        return SecItemAdd(query.merging(attrs) { _, new in new } as CFDictionary, nil)
    }

    static func forget() -> OSStatus {
        let status = SecItemDelete(query as CFDictionary)
        return status == errSecItemNotFound ? errSecSuccess : status
    }
}
