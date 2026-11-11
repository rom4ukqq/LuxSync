import Foundation
import Darwin

func ipv4(_ address: String) -> in_addr? {
    var value = in_addr()
    guard inet_pton(AF_INET, address, &value) == 1, value.s_addr != 0 else { return nil }
    return value
}

// Main queue only.
final class DatagramSocket {
    private let fd: Int32
    private let source: DispatchSourceRead
    private let destination: sockaddr_in
    var onPacket: ((Data, sockaddr_in) -> Void)?

    init?(host: String) {
        guard let addr = ipv4(host.isEmpty ? "255.255.255.255" : host) else { return nil }
        let socket = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard socket >= 0 else { return nil }
        var flag: Int32 = 1
        guard fcntl(socket, F_SETFL, O_NONBLOCK) == 0,
              setsockopt(socket, SOL_SOCKET, SO_BROADCAST, &flag, socklen_t(MemoryLayout<Int32>.size)) == 0
              else { close(socket); return nil }
        // Bind before registering the read source, including on the first poll.
        var local = sockaddr_in()
        local.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        local.sin_family = sa_family_t(AF_INET)
        let bound = withUnsafePointer(to: &local) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.bind(socket, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bound == 0 else { close(socket); return nil }
        var target = local
        target.sin_addr = addr
        target.sin_port = UInt16(47632).bigEndian
        destination = target
        fd = socket
        source = DispatchSource.makeReadSource(fileDescriptor: socket, queue: .main)
        source.setEventHandler { [weak self] in self?.read() }
        source.setCancelHandler { close(socket) }
        source.resume()
    }

    deinit { source.cancel() }

    func send(_ data: Data, to target: sockaddr_in? = nil) {
        var address = target ?? destination
        data.withUnsafeBytes { bytes in
            withUnsafePointer(to: &address) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    _ = sendto(fd, bytes.baseAddress, bytes.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
    }

    private func read() {
        for _ in 0..<32 {
            var from = sockaddr_in()
            var length = socklen_t(MemoryLayout<sockaddr_in>.size)
            // One extra byte so truncated overlength datagrams cannot look valid.
            var buffer = [UInt8](repeating: 0, count: Packet.size + 1)
            let n = withUnsafeMutablePointer(to: &from) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    recvfrom(fd, &buffer, buffer.count, 0, $0, &length)
                }
            }
            guard n >= 0 else { return }
            guard n < buffer.count, from.sin_family == AF_INET, from.sin_port == destination.sin_port,
                  destination.sin_addr.s_addr == INADDR_BROADCAST ||
                    from.sin_addr.s_addr == destination.sin_addr.s_addr else { continue }
            onPacket?(Data(buffer.prefix(n)), from)
        }
    }
}
