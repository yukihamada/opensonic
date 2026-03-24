import Foundation
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

/// Low-level BSD socket connection to the Soluna relay server.
///
/// Uses `socket(AF_INET, SOCK_DGRAM, 0)` with `recvfrom` on a background thread,
/// matching the C++ implementation exactly. NWConnection is intentionally NOT used
/// because it does not work reliably for this protocol.
public final class RelayConnection: @unchecked Sendable {

    // MARK: - Properties

    private var udpSocket: Int32 = -1
    private var relayAddr = sockaddr_in()
    private var recvThread: Thread?
    private var heartbeatTimer: Timer?
    private let running = AtomicFlag()

    private let channel: String
    private let host: String
    private let port: UInt16
    private let deviceName: String

    /// Called on the recv thread when an audio packet arrives.
    public var onPacket: ((Data) -> Void)?

    /// Called on the recv thread when a text control message arrives.
    public var onControlMessage: ((String) -> Void)?

    // MARK: - Init

    public init(channel: String, host: String, port: UInt16, deviceName: String) {
        self.channel = channel
        self.host = host
        self.port = port
        self.deviceName = deviceName
    }

    deinit {
        disconnect()
    }

    // MARK: - Connect

    /// Open the UDP socket, send HELLO/JOIN, and start the receive loop.
    /// Returns false if DNS resolution or socket creation fails.
    @discardableResult
    public func connect() -> Bool {
        guard !running.value else { return true }

        // DNS resolve
        var hints = addrinfo()
        hints.ai_family = AF_INET
        hints.ai_socktype = SOCK_DGRAM
        var res: UnsafeMutablePointer<addrinfo>?
        let portStr = "\(port)"

        guard getaddrinfo(host, portStr, &hints, &res) == 0, let addrInfo = res else {
            return false
        }
        memcpy(&relayAddr, addrInfo.pointee.ai_addr, Int(addrInfo.pointee.ai_addrlen))
        freeaddrinfo(res)

        // Create UDP socket
        udpSocket = socket(AF_INET, SOCK_DGRAM, 0)
        guard udpSocket >= 0 else { return false }

        // Set receive timeout (1 second)
        var tv = timeval(tv_sec: 1, tv_usec: 0)
        setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        // Send HELLO x3 (100ms apart)
        let hello = "HELLO\n"
        for i in 0..<3 {
            sendMessage(hello)
            if i < 2 { usleep(100_000) }
        }

        // JOIN:<group>::<device_name>\n
        let joinMsg = "JOIN:\(channel)::\(deviceName)\n"
        sendMessage(joinMsg)

        running.set(true)

        // Start receive thread
        recvThread = Thread { [weak self] in
            self?.recvLoop()
        }
        recvThread?.qualityOfService = .userInteractive
        recvThread?.start()

        // Heartbeat every 4 seconds on main run loop
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.heartbeatTimer = Timer.scheduledTimer(withTimeInterval: OSTConstants.heartbeatInterval, repeats: true) { [weak self] _ in
                guard let self, self.running.value, self.udpSocket >= 0 else { return }
                self.sendMessage("HELLO\n")
                self.sendMessage("JOIN:\(self.channel)::\(self.deviceName)\n")
            }
        }

        return true
    }

    // MARK: - Disconnect

    public func disconnect() {
        running.set(false)

        DispatchQueue.main.async { [weak self] in
            self?.heartbeatTimer?.invalidate()
            self?.heartbeatTimer = nil
        }

        if udpSocket >= 0 {
            Darwin.close(udpSocket)
            udpSocket = -1
        }
    }

    // MARK: - Send

    private func sendMessage(_ message: String) {
        guard udpSocket >= 0 else { return }
        message.withCString { ptr in
            withUnsafePointer(to: relayAddr) { addrPtr in
                let sockaddrPtr = UnsafeRawPointer(addrPtr).assumingMemoryBound(to: sockaddr.self)
                _ = sendto(udpSocket, ptr, strlen(ptr), 0, sockaddrPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }

    /// Send raw binary data (e.g. OSTP audio packets) to the relay server.
    ///
    /// Used by MicTransmitter and DJDeck to send encoded audio packets.
    public func sendRawData(_ data: Data) {
        guard udpSocket >= 0 else { return }
        data.withUnsafeBytes { ptr in
            guard let base = ptr.baseAddress else { return }
            withUnsafePointer(to: relayAddr) { addrPtr in
                let sockaddrPtr = UnsafeRawPointer(addrPtr).assumingMemoryBound(to: sockaddr.self)
                _ = sendto(udpSocket, base, data.count, 0, sockaddrPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }

    /// Send a text control message to the relay (e.g. JOIN, HELLO).
    public func sendControlMessage(_ message: String) {
        sendMessage(message)
    }

    // MARK: - Receive Loop

    private func recvLoop() {
        var buf = [UInt8](repeating: 0, count: OSTConstants.recvBufferSize)
        var sender = sockaddr_in()
        var senderLen = socklen_t(MemoryLayout<sockaddr_in>.size)

        while running.value && udpSocket >= 0 {
            let n = withUnsafeMutablePointer(to: &sender) { senderPtr -> Int in
                senderPtr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockaddrPtr in
                    recvfrom(udpSocket, &buf, buf.count, 0, sockaddrPtr, &senderLen)
                }
            }
            if n <= 0 { continue }

            // Skip packets too small for RTP
            if n < OSTConstants.rtpHeaderSize { continue }

            // RTP/OSTP audio packet: (byte[0] & 0xC0) == 0x80
            if (buf[0] & 0xC0) == 0x80 {
                let data = Data(bytes: buf, count: n)
                onPacket?(data)
            } else {
                // Text control message
                if let msg = String(bytes: buf[0..<n], encoding: .utf8) {
                    onControlMessage?(msg)
                }
            }
        }
    }
}

// MARK: - AtomicFlag

/// A simple thread-safe boolean flag using os_unfair_lock.
final class AtomicFlag: @unchecked Sendable {
    private var _value = false
    private var lock = os_unfair_lock()

    var value: Bool {
        os_unfair_lock_lock(&lock)
        defer { os_unfair_lock_unlock(&lock) }
        return _value
    }

    func set(_ newValue: Bool) {
        os_unfair_lock_lock(&lock)
        _value = newValue
        os_unfair_lock_unlock(&lock)
    }
}
