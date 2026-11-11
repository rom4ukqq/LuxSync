import Foundation
import Darwin
import CryptoKit

// Main queue only. A fresh socket clears old routes and pending exchanges.
final class Sender {
    private let host: String
    private let retryAfter: TimeInterval
    private var socket: DatagramSocket?
    private let key: SymmetricKey
    private var nonce: Data?
    private var sample: UInt32 = 0
    private var reply: (sockaddr_in, Data)?
    private var deadline: TimeInterval = 0
    private var lastProgress: TimeInterval = 0
    private(set) var lastAck: TimeInterval?
    var onAck: (() -> Void)?

    init(key: Data, host: String, retryAfter: TimeInterval) {
        self.host = host
        self.retryAfter = retryAfter
        self.key = SymmetricKey(data: key)
    }

    func resetConnection() {
        socket = nil
        nonce = nil
        reply = nil
        lastAck = nil
    }

    func poll(_ percent: Double) {
        guard let percent = validatedBrightness(percent) else { return }
        let now = ProcessInfo.processInfo.systemUptime
        if now - lastProgress >= retryAfter { resetConnection() }
        if socket == nil {
            socket = DatagramSocket(host: host)
            socket?.onPacket = { [weak self] data, from in self?.receive(data, from: from) }
            lastProgress = now
        }
        guard let socket else { return }
        sample = UInt32((percent * 100).rounded())
        if nonce == nil || now >= deadline {
            guard let fresh = randomNonce() else { return }
            nonce = fresh
            reply = nil
            deadline = now + 4
        }
        sendPending(using: socket)
    }

    private func sendPending(using socket: DatagramSocket) {
        guard let nonce else { return }
        if let (peer, challenge) = reply {
            if !socket.send(Packet(.sample, nonce, challenge, sample).encode(key: key), to: peer) {
                resetConnection()
            }
        } else if !socket.send(Packet(.hello, nonce).encode(key: key)) {
            resetConnection()
        }
    }

    private func receive(_ data: Data, from: sockaddr_in) {
        let now = ProcessInfo.processInfo.systemUptime
        guard now < deadline, let socket,
              let nonce, let packet = Packet(data, key: key), packet.sender == nonce else { return }
        if packet.kind == .challenge, reply == nil {
            reply = (from, packet.receiver)
            sendPending(using: socket)
        } else if packet.kind == .ack, let (peer, challenge) = reply,
                  from.sin_addr.s_addr == peer.sin_addr.s_addr, packet.receiver == challenge {
            self.nonce = nil
            lastProgress = now
            lastAck = now
            onAck?()
        }
    }
}
