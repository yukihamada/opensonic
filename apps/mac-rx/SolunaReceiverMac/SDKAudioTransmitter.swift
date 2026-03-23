import Foundation
import AVFoundation
import CoreMedia
import Darwin

@MainActor
final class SDKAudioTransmitter: ObservableObject {
    static let shared = SDKAudioTransmitter()

    @Published var isTransmitting = false {
        didSet { _isTransmittingAtomic = isTransmitting }
    }
    @Published var micEnabled = false {
        didSet { _micEnabledAtomic = micEnabled }
    }
    nonisolated(unsafe) private var _isTransmittingAtomic = false
    nonisolated(unsafe) private var _micEnabledAtomic = false
    @Published var channel: String = ""
    nonisolated(unsafe) var packetsSent: UInt64 = 0

    private var engine: AVAudioEngine?
    nonisolated(unsafe) private var udpSocket: Int32 = -1
    nonisolated(unsafe) private var relayAddr = sockaddr_in()
    nonisolated(unsafe) private var lanSocket: Int32 = -1
    nonisolated(unsafe) private var seqNum: UInt16 = 0
    nonisolated(unsafe) private var timestamp: UInt32 = 0
    nonisolated(unsafe) private let ssrc: UInt32 = UInt32.random(in: 0...UInt32.max)
    nonisolated(unsafe) private let samplesPerPacket = 96  // 2ms at 48kHz
    private var heartbeatTimer: Timer?

    // Bonjour + Unicast LAN: listeners register via UDP, we fan-out packets
    nonisolated(unsafe) private var listeners: [(sockaddr_in, Date)] = []
    nonisolated(unsafe) private var listenerLock = os_unfair_lock()
    nonisolated(unsafe) private var registrationSocket: Int32 = -1
    private var bonjourService: NetService?
    private var registrationThread: Thread?
    private var listenerCleanupTimer: Timer?

    func toggleMic() {
        micEnabled.toggle()
        print("[SDKTx] MIC toggled: \(micEnabled), atomic: \(_micEnabledAtomic)")
    }

    func start(channel: String, micEnabled: Bool = false, host: String = "relay.solun.art", port: UInt16 = 5100) {
        guard !isTransmitting else { return }
        self.channel = channel
        self.micEnabled = micEnabled

        // DNS resolve
        var hints = addrinfo()
        hints.ai_family = AF_INET
        hints.ai_socktype = SOCK_DGRAM
        var res: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, "\(port)", &hints, &res) == 0, let info = res else {
            print("[SDKTx] DNS failed for \(host)")
            return
        }
        memcpy(&relayAddr, info.pointee.ai_addr, Int(info.pointee.ai_addrlen))
        freeaddrinfo(res)

        // Create relay UDP socket
        udpSocket = socket(AF_INET, SOCK_DGRAM, 0)
        guard udpSocket >= 0 else { print("[SDKTx] socket() failed"); return }

        // Create LAN unicast socket for sending to registered listeners
        lanSocket = socket(AF_INET, SOCK_DGRAM, 0)
        if lanSocket >= 0 {
            print("[SDKTx] LAN unicast socket ready")
        }

        // Start Bonjour advertisement + registration listener
        startBonjourAndRegistration(channel: channel)

        // Send JOIN to relay
        let deviceName = Host.current().localizedName ?? "SolunaSDK-Mac"
        sendUDP("JOIN:\(channel)::\(deviceName)\n")

        // Wait briefly for OK:joined
        var tv = timeval(tv_sec: 0, tv_usec: 500000)  // 500ms
        setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        var buf = [UInt8](repeating: 0, count: 256)
        let n = recv(udpSocket, &buf, buf.count, 0)
        if n > 0, let response = String(bytes: buf[0..<n], encoding: .utf8) {
            print("[SDKTx] JOIN response: \(response.trimmingCharacters(in: .whitespacesAndNewlines))")
        }

        // Clear recv timeout for non-blocking send
        tv = timeval(tv_sec: 0, tv_usec: 0)
        setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        // Start audio engine with mic input (simple: native format, no conversion)
        let engine = AVAudioEngine()
        let inputNode = engine.inputNode
        let inputFormat = inputNode.outputFormat(forBus: 0)

        print("[SDKTx] Mic: \(inputFormat.sampleRate)Hz, \(inputFormat.channelCount)ch")

        var sampleBuffer = [Float]()
        let spp = samplesPerPacket
        let channels = Int(inputFormat.channelCount)

        inputNode.installTap(onBus: 0, bufferSize: 1024, format: inputFormat) { [weak self] buffer, _ in
            guard let self, self._isTransmittingAtomic else { return }
            guard self._micEnabledAtomic else { return }

            // Extract first channel (mono)
            guard let data = buffer.floatChannelData?[0] else { return }
            for i in 0..<Int(buffer.frameLength) {
                sampleBuffer.append(data[i])
            }

            // Send packets when we have enough samples
            while sampleBuffer.count >= spp {
                let samples = Array(sampleBuffer.prefix(spp))
                sampleBuffer.removeFirst(spp)
                self.sendAudioPacket(samples)
            }
        }

        do {
            try engine.start()
            self.engine = engine
            isTransmitting = true

            // Heartbeat every 5s
            heartbeatTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in
                self?.sendUDP("HELLO\n")
            }

            // Cleanup stale listeners every 10s
            listenerCleanupTimer = Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in
                self?.cleanupStaleListeners()
            }

            print("[SDKTx] Started transmitting on channel '\(channel)'")
        } catch {
            print("[SDKTx] Engine start error: \(error)")
            Darwin.close(udpSocket)
            udpSocket = -1
        }
    }

    func stop() {
        guard isTransmitting else { return }
        isTransmitting = false
        heartbeatTimer?.invalidate()
        heartbeatTimer = nil
        listenerCleanupTimer?.invalidate()
        listenerCleanupTimer = nil
        stopBonjourAndRegistration()
        engine?.inputNode.removeTap(onBus: 0)
        engine?.stop()
        engine = nil
        if udpSocket >= 0 { Darwin.close(udpSocket); udpSocket = -1 }
        if lanSocket >= 0 { Darwin.close(lanSocket); lanSocket = -1 }
        packetsSent = 0
        print("[SDKTx] Stopped")
    }

    // MARK: - RTP Packet Construction

    nonisolated private func sendAudioPacket(_ samples: [Float]) {
        guard udpSocket >= 0 else { return }

        // RTP header (12 bytes) + payload + CRC (4 bytes)
        let payloadSize = samples.count * 4  // S24-in-S32LE = 4 bytes per sample
        var packet = [UInt8](repeating: 0, count: 12 + payloadSize + 4)

        // RTP header
        packet[0] = 0x80  // V=2
        packet[1] = 96    // PT=96 (S24)
        packet[2] = UInt8((seqNum >> 8) & 0xFF)
        packet[3] = UInt8(seqNum & 0xFF)
        packet[4] = UInt8((timestamp >> 24) & 0xFF)
        packet[5] = UInt8((timestamp >> 16) & 0xFF)
        packet[6] = UInt8((timestamp >> 8) & 0xFF)
        packet[7] = UInt8(timestamp & 0xFF)
        packet[8] = UInt8((ssrc >> 24) & 0xFF)
        packet[9] = UInt8((ssrc >> 16) & 0xFF)
        packet[10] = UInt8((ssrc >> 8) & 0xFF)
        packet[11] = UInt8(ssrc & 0xFF)

        // Encode S24-in-S32LE payload
        let scale: Float = 8388608.0  // 2^23
        for i in 0..<samples.count {
            let s24 = Int32(max(-1.0, min(1.0, samples[i])) * scale)
            let offset = 12 + i * 4
            packet[offset] = UInt8(truncatingIfNeeded: s24)
            packet[offset + 1] = UInt8(truncatingIfNeeded: s24 >> 8)
            packet[offset + 2] = UInt8(truncatingIfNeeded: s24 >> 16)
            packet[offset + 3] = UInt8(truncatingIfNeeded: s24 >> 24)
        }

        // CRC-32 trailer (last 4 bytes)
        let crc = crc32(packet, count: 12 + payloadSize)
        let crcOffset = 12 + payloadSize
        packet[crcOffset] = UInt8(truncatingIfNeeded: crc)
        packet[crcOffset + 1] = UInt8(truncatingIfNeeded: crc >> 8)
        packet[crcOffset + 2] = UInt8(truncatingIfNeeded: crc >> 16)
        packet[crcOffset + 3] = UInt8(truncatingIfNeeded: crc >> 24)

        // Send to relay (WAN)
        packet.withUnsafeBufferPointer { buf in
            withUnsafePointer(to: relayAddr) { addr in
                let sa = UnsafeRawPointer(addr).assumingMemoryBound(to: sockaddr.self)
                _ = sendto(udpSocket, buf.baseAddress!, packet.count, 0, sa,
                          socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        // Send to LAN listeners via unicast (low latency, same WiFi)
        if lanSocket >= 0 {
            os_unfair_lock_lock(&listenerLock)
            let currentListeners = listeners
            os_unfair_lock_unlock(&listenerLock)
            for (listenerAddr, _) in currentListeners {
                var addr = listenerAddr
                packet.withUnsafeBufferPointer { buf in
                    withUnsafePointer(to: &addr) { a in
                        let sa = UnsafeRawPointer(a).assumingMemoryBound(to: sockaddr.self)
                        _ = sendto(lanSocket, buf.baseAddress!, packet.count, 0, sa,
                                  socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }
            }
        }

        seqNum &+= 1
        timestamp &+= UInt32(samples.count)
        packetsSent += 1
    }

    /// Send system audio from ScreenCaptureKit CMSampleBuffer (called from audio callback)
    /// Send system audio from ScreenCaptureKit (skipped when mic is ON — mic takes priority)
    nonisolated func sendSystemAudio(_ sampleBuffer: CMSampleBuffer) {
        guard udpSocket >= 0 else { return }
        guard !_micEnabledAtomic else { return }  // Mic ON → mic audio takes priority
        guard let blockBuffer = sampleBuffer.dataBuffer else { return }
        var length = 0
        var dataPointer: UnsafeMutablePointer<Int8>?
        CMBlockBufferGetDataPointer(blockBuffer, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &length, dataPointerOut: &dataPointer)
        guard let dataPointer, length > 0 else { return }

        let floatPtr = UnsafeRawPointer(dataPointer).assumingMemoryBound(to: Float.self)
        let floatCount = length / MemoryLayout<Float>.size
        let channels = 2  // ScreenCaptureKit delivers interleaved stereo
        let frames = floatCount / channels

        // Extract mono (left channel) and send packets
        let spp = samplesPerPacket
        var offset = 0
        while offset + spp <= frames {
            var mono = [Float](repeating: 0, count: spp)
            for i in 0..<spp {
                mono[i] = floatPtr[(offset + i) * channels]
                // Mix mic if enabled
                // (mic audio comes from the engine tap, system audio from here)
            }
            sendAudioPacket(mono)
            offset += spp
        }
    }

    nonisolated private func crc32(_ data: [UInt8], count: Int) -> UInt32 {
        var crc: UInt32 = 0xFFFFFFFF
        for i in 0..<count {
            crc ^= UInt32(data[i])
            for _ in 0..<8 {
                crc = (crc >> 1) ^ (crc & 1 != 0 ? 0xEDB88320 : 0)
            }
        }
        return crc ^ 0xFFFFFFFF
    }

    private func sendUDP(_ message: String) {
        guard udpSocket >= 0 else { return }
        message.withCString { ptr in
            withUnsafePointer(to: relayAddr) { addr in
                let sa = UnsafeRawPointer(addr).assumingMemoryBound(to: sockaddr.self)
                _ = sendto(udpSocket, ptr, strlen(ptr), 0, sa,
                          socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }

    // MARK: - Bonjour Advertisement + Listener Registration

    private func startBonjourAndRegistration(channel: String) {
        // 1. Start registration socket on port 5005 to receive LISTEN messages from iPhones
        registrationSocket = socket(AF_INET, SOCK_DGRAM, 0)
        guard registrationSocket >= 0 else {
            print("[SDKTx] Failed to create registration socket")
            return
        }

        var bindAddr = sockaddr_in()
        bindAddr.sin_family = sa_family_t(AF_INET)
        bindAddr.sin_port = UInt16(5005).bigEndian
        bindAddr.sin_addr.s_addr = INADDR_ANY.bigEndian

        var yes: Int32 = 1
        setsockopt(registrationSocket, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))

        let bindResult = withUnsafePointer(to: &bindAddr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                Darwin.bind(registrationSocket, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        guard bindResult == 0 else {
            print("[SDKTx] Registration socket bind failed (port 5005): \(errno)")
            Darwin.close(registrationSocket)
            registrationSocket = -1
            return
        }

        // Set 500ms recv timeout so thread can check isTransmitting periodically
        var tv = timeval(tv_sec: 0, tv_usec: 500000)
        setsockopt(registrationSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        print("[SDKTx] Registration socket listening on port 5005")

        // 2. Start background thread to receive LISTEN messages
        let thread = Thread { [weak self] in
            self?.registrationLoop()
        }
        thread.name = "SDKTx-Registration"
        thread.qualityOfService = .userInteractive
        thread.start()
        registrationThread = thread

        // 3. Advertise Bonjour service: _soluna-tx._udp with channel name as TXT record
        let service = NetService(domain: "local.", type: "_soluna-tx._udp.", name: channel, port: 5005)
        let txtData = NetService.data(fromTXTRecord: ["channel": channel.data(using: .utf8) ?? Data()])
        service.setTXTRecord(txtData)
        service.publish()
        bonjourService = service
        print("[SDKTx] Bonjour advertising _soluna-tx._udp. channel=\(channel)")
    }

    private func stopBonjourAndRegistration() {
        // Stop Bonjour
        bonjourService?.stop()
        bonjourService = nil

        // Close registration socket (causes registrationLoop to exit)
        if registrationSocket >= 0 {
            Darwin.close(registrationSocket)
            registrationSocket = -1
        }
        registrationThread = nil

        // Clear listeners
        os_unfair_lock_lock(&listenerLock)
        listeners.removeAll()
        os_unfair_lock_unlock(&listenerLock)

        print("[SDKTx] Bonjour + registration stopped")
    }

    /// Background thread: receives "LISTEN:<port>:<channel>\n" UDP messages from iPhones
    nonisolated private func registrationLoop() {
        var buf = [UInt8](repeating: 0, count: 256)
        var sender = sockaddr_in()
        var senderLen = socklen_t(MemoryLayout<sockaddr_in>.size)

        while registrationSocket >= 0 {
            senderLen = socklen_t(MemoryLayout<sockaddr_in>.size)
            let n = withUnsafeMutablePointer(to: &sender) { sp -> Int in
                sp.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    Darwin.recvfrom(registrationSocket, &buf, buf.count, 0, sa, &senderLen)
                }
            }

            guard n > 0 else { continue }

            guard let msg = String(bytes: buf[0..<n], encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) else { continue }

            // Parse "LISTEN:<port>:<channel>"
            let parts = msg.split(separator: ":")
            guard parts.count >= 2, parts[0] == "LISTEN" else { continue }
            guard let listenerPort = UInt16(parts[1]) else { continue }

            // Build listener address: sender's IP + specified port
            var listenerAddr = sender
            listenerAddr.sin_port = listenerPort.bigEndian

            let now = Date()
            var ipStr = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
            inet_ntop(AF_INET, &listenerAddr.sin_addr, &ipStr, socklen_t(INET_ADDRSTRLEN))
            let ipString = String(cString: ipStr)

            os_unfair_lock_lock(&listenerLock)
            // Update existing or add new listener
            if let idx = listeners.firstIndex(where: {
                $0.0.sin_addr.s_addr == listenerAddr.sin_addr.s_addr && $0.0.sin_port == listenerAddr.sin_port
            }) {
                listeners[idx].1 = now
            } else {
                listeners.append((listenerAddr, now))
                print("[SDKTx] New LAN listener: \(ipString):\(listenerPort) (total: \(listeners.count))")
            }
            os_unfair_lock_unlock(&listenerLock)
        }
    }

    /// Remove listeners that haven't re-registered in 30 seconds
    nonisolated private func cleanupStaleListeners() {
        let cutoff = Date().addingTimeInterval(-30)
        os_unfair_lock_lock(&listenerLock)
        let before = listeners.count
        listeners.removeAll { $0.1 < cutoff }
        let after = listeners.count
        os_unfair_lock_unlock(&listenerLock)
        if before != after {
            print("[SDKTx] Cleaned up \(before - after) stale listener(s), \(after) remaining")
        }
    }
}
