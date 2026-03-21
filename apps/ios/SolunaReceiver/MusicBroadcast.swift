import AVFoundation
import Foundation
import UIKit

@MainActor
class MusicBroadcastManager: ObservableObject {
    static let shared = MusicBroadcastManager()

    @Published var isBroadcasting = false
    @Published var currentTrack: String = ""
    @Published var progress: Double = 0  // 0.0 - 1.0
    @Published var broadcastChannel: String = ""

    private var audioFile: AVAudioFile?
    private var udpSocket: Int32 = -1
    private var relayAddr = sockaddr_in()
    private var broadcastTimer: Timer?
    private var samplePosition: AVAudioFramePosition = 0

    // Start broadcasting an audio file to a channel
    func start(fileURL: URL, channel: String) {
        stop()

        guard let file = try? AVAudioFile(forReading: fileURL) else { return }
        audioFile = file
        broadcastChannel = channel
        currentTrack = fileURL.lastPathComponent
        isBroadcasting = true
        samplePosition = 0

        // Connect UDP to relay
        connectRelay(channel: channel)

        // Send 96 samples every 2ms (48000 Hz / 96 samples = 500 packets/sec)
        let samplesPerPacket = 96
        let interval = Double(samplesPerPacket) / 48000.0  // ~2ms

        broadcastTimer = Timer.scheduledTimer(withTimeInterval: interval, repeats: true) { [weak self] _ in
            self?.sendNextPacket(samplesPerPacket: samplesPerPacket)
        }
    }

    func stop() {
        broadcastTimer?.invalidate()
        broadcastTimer = nil
        if udpSocket >= 0 { Darwin.close(udpSocket); udpSocket = -1 }
        isBroadcasting = false
        audioFile = nil
        progress = 0
    }

    private func connectRelay(channel: String) {
        var hints = addrinfo()
        hints.ai_family = AF_INET
        hints.ai_socktype = SOCK_DGRAM
        var res: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo("relay.solun.art", "5100", &hints, &res) == 0, let info = res else { return }
        memcpy(&relayAddr, info.pointee.ai_addr, Int(info.pointee.ai_addrlen))
        freeaddrinfo(res)

        udpSocket = socket(AF_INET, SOCK_DGRAM, 0)
        guard udpSocket >= 0 else { return }

        // JOIN as DJ
        let deviceName = UIDevice.current.name
        let msg = "JOIN:\(channel)::\(deviceName)\n"
        sendUDP(msg)
    }

    private func sendNextPacket(samplesPerPacket: Int) {
        guard let file = audioFile, udpSocket >= 0 else { stop(); return }

        // Read samples from file
        let buffer = AVAudioPCMBuffer(pcmFormat: file.processingFormat, frameCapacity: AVAudioFrameCount(samplesPerPacket))!
        do {
            try file.read(into: buffer, frameCount: AVAudioFrameCount(samplesPerPacket))
        } catch {
            // End of file
            stop()
            return
        }

        guard buffer.frameLength > 0 else { stop(); return }

        // Convert to S24-in-S32LE mono
        let frameCount = Int(buffer.frameLength)
        var packet = [UInt8]()

        // RTP header (12 bytes)
        packet.append(0x80)  // V=2, no padding, no extension
        packet.append(96)    // PT=96 (S24)
        // Sequence number (simple counter)
        let seq = UInt16(samplePosition / Int64(samplesPerPacket)) & 0xFFFF
        packet.append(UInt8(seq >> 8))
        packet.append(UInt8(seq & 0xFF))
        // Timestamp
        let ts = UInt32(samplePosition & 0xFFFFFFFF)
        packet.append(contentsOf: withUnsafeBytes(of: ts.bigEndian) { Array($0) })
        // SSRC
        packet.append(contentsOf: [0x00, 0x00, 0x00, 0x01])

        // PCM payload: convert float to S24-in-S32LE
        if let channelData = buffer.floatChannelData?[0] {
            for i in 0..<frameCount {
                let sample = channelData[i]
                let scaled = Int32(max(-1.0, min(1.0, sample)) * 8388607.0)
                packet.append(UInt8(truncatingIfNeeded: scaled))
                packet.append(UInt8(truncatingIfNeeded: scaled >> 8))
                packet.append(UInt8(truncatingIfNeeded: scaled >> 16))
                packet.append(UInt8(truncatingIfNeeded: scaled >> 24))
            }
        }

        // CRC placeholder (4 bytes)
        packet.append(contentsOf: [0, 0, 0, 0])

        // Send
        packet.withUnsafeBytes { ptr in
            withUnsafePointer(to: relayAddr) { addrPtr in
                let sa = UnsafeRawPointer(addrPtr).assumingMemoryBound(to: sockaddr.self)
                _ = sendto(udpSocket, ptr.baseAddress, packet.count, 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        samplePosition += Int64(frameCount)
        progress = Double(samplePosition) / Double(file.length)

        // Heartbeat every ~5 seconds (2500 packets)
        if samplePosition % (Int64(samplesPerPacket) * 2500) == 0 {
            sendUDP("HELLO\n")
        }
    }

    private func sendUDP(_ msg: String) {
        guard udpSocket >= 0 else { return }
        msg.withCString { ptr in
            withUnsafePointer(to: relayAddr) { addrPtr in
                let sa = UnsafeRawPointer(addrPtr).assumingMemoryBound(to: sockaddr.self)
                _ = sendto(udpSocket, ptr, strlen(ptr), 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }
}
