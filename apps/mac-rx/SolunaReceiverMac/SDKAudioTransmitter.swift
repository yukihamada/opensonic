import Foundation
import AVFoundation
import CoreMedia
import Darwin

@MainActor
final class SDKAudioTransmitter: ObservableObject {
    static let shared = SDKAudioTransmitter()

    @Published var isTransmitting = false
    @Published var micEnabled = false {
        didSet { _micEnabledAtomic = micEnabled }
    }
    nonisolated(unsafe) private var _micEnabledAtomic = false
    @Published var channel: String = ""
    nonisolated(unsafe) var packetsSent: UInt64 = 0

    private var engine: AVAudioEngine?
    nonisolated(unsafe) private var udpSocket: Int32 = -1
    nonisolated(unsafe) private var relayAddr = sockaddr_in()
    nonisolated(unsafe) private var lanSocket: Int32 = -1
    nonisolated(unsafe) private var lanAddr = sockaddr_in()
    nonisolated(unsafe) private var seqNum: UInt16 = 0
    nonisolated(unsafe) private var timestamp: UInt32 = 0
    nonisolated(unsafe) private let ssrc: UInt32 = UInt32.random(in: 0...UInt32.max)
    nonisolated(unsafe) private let samplesPerPacket = 96  // 2ms at 48kHz
    private var heartbeatTimer: Timer?

    func toggleMic() {
        micEnabled.toggle()
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

        // Create LAN multicast socket (239.69.0.1:5004)
        lanSocket = socket(AF_INET, SOCK_DGRAM, 0)
        if lanSocket >= 0 {
            lanAddr.sin_family = sa_family_t(AF_INET)
            lanAddr.sin_port = UInt16(5004).bigEndian
            inet_pton(AF_INET, "239.69.0.1", &lanAddr.sin_addr)
            // Set TTL for multicast
            var ttl: UInt8 = 1
            setsockopt(lanSocket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, socklen_t(MemoryLayout<UInt8>.size))
            print("[SDKTx] LAN multicast ready (239.69.0.1:5004)")
        }

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
            guard let self, self.isTransmitting else { return }
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

        // Send to LAN multicast (low latency, same WiFi)
        if lanSocket >= 0 {
            packet.withUnsafeBufferPointer { buf in
                withUnsafePointer(to: lanAddr) { addr in
                    let sa = UnsafeRawPointer(addr).assumingMemoryBound(to: sockaddr.self)
                    _ = sendto(lanSocket, buf.baseAddress!, packet.count, 0, sa,
                              socklen_t(MemoryLayout<sockaddr_in>.size))
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
}
