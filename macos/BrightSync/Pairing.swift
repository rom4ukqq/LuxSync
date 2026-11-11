import Foundation
import Security

enum BrightSyncKeychain {
    private static func query(_ account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: "com.brightsync.sender",
         kSecAttrAccount as String: account]
    }

    static func load(_ account: String) -> Data? {
        var found: CFTypeRef?
        var request = query(account)
        request[kSecReturnData as String] = true
        guard SecItemCopyMatching(request as CFDictionary, &found) == errSecSuccess,
              let data = found as? Data else { return nil }
        return data
    }

    static func save(_ secret: Data, account: String) -> OSStatus {
        let query = query(account)
        let attrs = [kSecValueData as String: secret]
        let status = SecItemUpdate(query as CFDictionary, attrs as CFDictionary)
        if status != errSecItemNotFound { return status }
        return SecItemAdd(query.merging(attrs) { _, new in new } as CFDictionary, nil)
    }

    static func forget(_ account: String) -> OSStatus {
        let status = SecItemDelete(query(account) as CFDictionary)
        return status == errSecItemNotFound ? errSecSuccess : status
    }
}

enum Pairing {
    private static let account = "paired-windows"

    static func load() -> Data? {
        guard let key = BrightSyncKeychain.load(account), key.count == 32 else { return nil }
        return key
    }

    static func save(_ key: Data) -> OSStatus { BrightSyncKeychain.save(key, account: account) }
    static func forget() -> OSStatus { BrightSyncKeychain.forget(account) }
}
