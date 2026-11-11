import Foundation
import Darwin

private let pinAlphabet = Set("ABCDEFGHJKLMNPQRSTUVWXYZ23456789".utf8)

func normalizedPin(_ input: String) -> [UInt8]? {
    let pin = input.utf8.compactMap { byte -> UInt8? in
        if byte == 32 || byte == 45 { return nil }
        return (97...122).contains(byte) ? byte - 32 : byte
    }
    return pin.count == 8 && pin.allSatisfy(pinAlphabet.contains) ? pin : nil
}

// One SPAKE2 attempt, with UDP retries for each step. The PIN stays local.
final class PairingClient {
    private let socket: DatagramSocket
    private let nonce: Data
    private let offerRequest: Data
    private var context: OpaquePointer?
    private var result: [UInt8]?
    private var peer: sockaddr_in?
    private var timer: Timer?
    private var deadline: TimeInterval = 0
    private var completion: ((Data?) -> Void)?
    private let header = Data([66, 83, 80, 82, 1]) // BSPR v1

    init?(pin: [UInt8], host: String) {
        guard pin.count == 8, let socket = DatagramSocket(host: host), let nonce = randomNonce() else { return nil }
        var message = [UInt8](repeating: 0, count: 32)
        guard let context = bs_pake_start(1, pin, &message) else { return nil }
        self.socket = socket
        self.nonce = nonce
        self.context = context
        offerRequest = Data([66, 83, 80, 82, 1, 1]) + nonce + Data(message)
        socket.onPacket = { [weak self] data, from in self?.receive(data, from: from) }
    }

    deinit {
        timer?.invalidate()
        if let context { bs_pake_free(context) }
        result?.withUnsafeMutableBytes { bs_pake_clear($0.baseAddress, $0.count) }
    }

    func start(_ completion: @escaping (Data?) -> Void) {
        self.completion = completion
        deadline = ProcessInfo.processInfo.systemUptime + 10
        let timer = Timer(timeInterval: 0.7, repeats: true) { [weak self] _ in self?.send() }
        self.timer = timer
        RunLoop.main.add(timer, forMode: .common)
        send()
    }

    func cancel() {
        timer?.invalidate()
        timer = nil
        completion = nil
    }

    private func finish(_ key: Data?) {
        let callback = completion
        cancel()
        callback?(key)
    }

    private func send() {
        guard ProcessInfo.processInfo.systemUptime < deadline else { finish(nil); return }
        if let result, let peer {
            socket.send(header + Data([3]) + nonce + Data(result[32..<64]), to: peer)
        } else {
            socket.send(offerRequest)
        }
    }

    private func receive(_ data: Data, from: sockaddr_in) {
        guard completion != nil, data.count == 54, data.prefix(5) == header,
              data[6..<22] == nonce else { return }
        let payload = Array(data[22..<54])
        if data[5] == 2, let context {
            var result = [UInt8](repeating: 0, count: 96)
            let ok = bs_pake_finish(context, payload, Array(nonce), &result)
            bs_pake_free(context)
            self.context = nil
            guard ok == 1 else { finish(nil); return }
            self.result = result
            peer = from
            send()
        } else if data[5] == 4, let result, let peer,
                  peer.sin_addr.s_addr == from.sin_addr.s_addr,
                  bs_pake_equal(payload, Array(result[64..<96])) == 1 {
            finish(Data(result.prefix(32)))
        }
    }
}
