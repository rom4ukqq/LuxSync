import Foundation
import CryptoKit
import Security

func randomNonce() -> Data? {
    var bytes = [UInt8](repeating: 0, count: 16)
    guard SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes) == errSecSuccess,
          bytes.contains(where: { $0 != 0 }) else { return nil }
    return Data(bytes)
}

struct Packet {
    enum Kind: UInt8 { case hello = 1, challenge, sample, ack }
    static let size = 76
    static let zero = Data(repeating: 0, count: 16)
    let kind: Kind
    let sender: Data
    let receiver: Data
    let millilux: UInt32

    func encode(key: SymmetricKey) -> Data {
        var data = Data([66, 83, 89, 78, 1, kind.rawValue, 0, 0])
        data.append(sender)
        data.append(receiver)
        for shift in [24, 16, 8, 0] { data.append(UInt8(truncatingIfNeeded: millilux >> shift)) }
        data.append(contentsOf: HMAC<SHA256>.authenticationCode(for: data, using: key))
        return data
    }

    init(_ kind: Kind, _ sender: Data, _ receiver: Data = Packet.zero, _ millilux: UInt32 = 0) {
        self.kind = kind; self.sender = sender; self.receiver = receiver; self.millilux = millilux
    }

    init?(_ data: Data, key: SymmetricKey) {
        let b = [UInt8](data)
        guard b.count == Self.size, Array(b[0..<5]) == [66, 83, 89, 78, 1],
              let kind = Kind(rawValue: b[5]), b[6] == 0, b[7] == 0,
              HMAC<SHA256>.isValidAuthenticationCode(b[44..<76], authenticating: b[0..<44], using: key)
              else { return nil }
        let sender = Data(b[8..<24]), receiver = Data(b[24..<40])
        let lux = b[40..<44].reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        guard sender != Self.zero, (receiver == Self.zero) == (kind == .hello),
              kind == .sample ? lux <= 250_000_000 : lux == 0 else { return nil }
        self.init(kind, sender, receiver, lux)
    }
}

func parseKey(_ input: String) -> Data? {
    let bytes = Array(input.trimmingCharacters(in: .whitespacesAndNewlines).utf8)
    func hex(_ b: UInt8) -> UInt8? {
        switch b {
        case 48...57: b - 48
        case 65...70: b - 55
        case 97...102: b - 87
        default: nil
        }
    }
    guard bytes.count == 64 else { return nil }
    var key = Data()
    for i in stride(from: 0, to: 64, by: 2) {
        guard let hi = hex(bytes[i]), let lo = hex(bytes[i + 1]) else { return nil }
        key.append(hi * 16 + lo)
    }
    return key.contains { $0 != 0 } ? key : nil
}
