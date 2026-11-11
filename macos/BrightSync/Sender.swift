import Foundation
import Darwin
import CryptoKit

// Main queue only. Each poll uses a fresh nonce.
final class Sender {
    private let socket: DatagramSocket
    private let key: SymmetricKey
    private var nonce: Data?
    private var sample: UInt32 = 0
    private var reply: (sockaddr_in, Data)?
    private var deadline: TimeInterval = 0
    private(set) var lastAck: TimeInterval?
    var onAck: (() -> Void)?

    init?(key: Data, host: String) {
        guard let socket = DatagramSocket(host: host) else { return nil }
        self.socket = socket
        self.key = SymmetricKey(data: key)
        socket.onPacket = { [weak self] data, from in self?.receive(data, from: from) }
    }

    func poll(_ lux: Double) {
        guard let lux = validatedLux(lux), let nonce = randomNonce() else { return }
        self.nonce = nonce
        sample = UInt32((lux * 1000).rounded())
        reply = nil
        deadline = ProcessInfo.processInfo.systemUptime + 4
        socket.send(Packet(.hello, nonce).encode(key: key))
    }

    private func receive(_ data: Data, from: sockaddr_in) {
        guard ProcessInfo.processInfo.systemUptime < deadline,
              let nonce, let packet = Packet(data, key: key), packet.sender == nonce else { return }
        if packet.kind == .challenge, reply == nil {
            reply = (from, packet.receiver)
            socket.send(Packet(.sample, nonce, packet.receiver, sample).encode(key: key), to: from)
        } else if packet.kind == .ack, let (peer, challenge) = reply,
                  from.sin_addr.s_addr == peer.sin_addr.s_addr, packet.receiver == challenge {
            self.nonce = nil
            lastAck = ProcessInfo.processInfo.systemUptime
            onAck?()
        }
    }
}
