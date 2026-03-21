//
//  SDKAudioReceiver.swift
//  SolunaReceiver
//
//  Pure-Swift relay audio receiver using AVAudioSourceNode.
//  Replaces the C++ bridge for relay playback.
//
//  Architecture: UDP recv thread -> decode -> ring buffer -> AVAudioSourceNode callback
//

import Foundation
import AVFoundation
import UIKit
import Darwin

// MARK: - SDKAudioReceiver

@MainActor
final class SDKAudioReceiver: ObservableObject {

    // MARK: - State

    enum State: String {
        case stopped = "Stopped"
        case connecting = "Connecting..."
        case receiving = "Receiving"
        case error = "Error"
    }

    @Published private(set) var state: State = .stopped
    @Published private(set) var isConnected = false
    @Published private(set) var isReceivingAudio = false
    @Published private(set) var packetsReceived: UInt64 = 0
    @Published var channel: String = "soluna"
    @Published var volume: Float = 1.0 {
        didSet { audioEngine?.mainMixerNode.outputVolume = volume }
    }

    var isPlaying: Bool { state == .receiving || state == .connecting }

    // MARK: - Audio Engine

    private var audioEngine: AVAudioEngine?
    private var sourceNode: AVAudioSourceNode?
    private let playbackFormat: AVAudioFormat

    // MARK: - Lock-Free Ring Buffer (SPSC)

    /// 4 seconds @ 48 kHz mono
    private let ringCapacity = 192_000
    private var ringBuffer: UnsafeMutablePointer<Float>
    private var writePos: Int64 = 0
    private var readPos: Int64 = 0
    /// 100 ms prefill = 4800 samples
    private let prefillThreshold = 4800
    private var prefilled = false

    // MARK: - Network

    private var udpSocket: Int32 = -1
    private var relayAddr = sockaddr_in()
    private let recvQueue = DispatchQueue(label: "com.soluna.sdkrecv", qos: .userInteractive)
    private var running = false
    private var heartbeatTimer: DispatchSourceTimer?

    // MARK: - ADPCM Step Table (IMA standard, 89 entries)

    private static let stepTable: [Int16] = [
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
        157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
        598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
        1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
        13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
    ]

    private static let indexTable: [Int8] = [
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8,
    ]

    // MARK: - Init / Deinit

    init() {
        playbackFormat = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: 48000,
            channels: 2,
            interleaved: false
        )!
        ringBuffer = .allocate(capacity: ringCapacity)
        ringBuffer.initialize(repeating: 0, count: ringCapacity)
    }

    deinit {
        // stop() must be called on MainActor; ensure cleanup in deinit
        ringBuffer.deallocate()
    }

    // MARK: - Public API

    func start(channel: String? = nil) {
        guard state == .stopped || state == .error else { return }
        if let ch = channel { self.channel = ch }

        state = .connecting
        packetsReceived = 0
        isReceivingAudio = false
        isConnected = false

        configureAudioSession()
        UIApplication.shared.isIdleTimerDisabled = true

        flushRing()
        startAudioEngine()
        connectRelay()
    }

    func stop() {
        running = false

        heartbeatTimer?.cancel()
        heartbeatTimer = nil

        if udpSocket >= 0 {
            Darwin.close(udpSocket)
            udpSocket = -1
        }

        stopAudioEngine()
        flushRing()

        state = .stopped
        isConnected = false
        isReceivingAudio = false

        UIApplication.shared.isIdleTimerDisabled = false
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    func setChannel(_ name: String) {
        let wasPlaying = isPlaying
        if wasPlaying { stop() }
        channel = name
        if wasPlaying { start() }
    }

    // MARK: - Ring Buffer (lock-free SPSC)

    private func ringAvailable() -> Int {
        let w = OSAtomicAdd64(0, &writePos)
        let r = OSAtomicAdd64(0, &readPos)
        return Int(w - r)
    }

    /// Write mono float samples into ring buffer. Called from recv thread.
    private func writeSamples(_ samples: UnsafePointer<Float>, count: Int) {
        let w = Int(OSAtomicAdd64(0, &writePos))
        let cap = ringCapacity
        // Drop if ring is nearly full (leave 10% headroom)
        if ringAvailable() > cap - cap / 10 { return }
        for i in 0..<count {
            ringBuffer[(w + i) % cap] = samples[i]
        }
        OSAtomicAdd64(Int64(count), &writePos)
    }

    /// Read mono float samples from ring buffer. Called from audio callback.
    private func ringRead(_ dst: UnsafeMutablePointer<Float>, count: Int) -> Int {
        let avail = ringAvailable()
        let n = min(avail, count)
        let r = Int(OSAtomicAdd64(0, &readPos))
        let cap = ringCapacity
        for i in 0..<n {
            dst[i] = ringBuffer[(r + i) % cap]
        }
        OSAtomicAdd64(Int64(n), &readPos)
        return n
    }

    private func flushRing() {
        writePos = 0
        readPos = 0
        prefilled = false
    }

    // MARK: - Audio Engine (AVAudioSourceNode)

    private func startAudioEngine() {
        guard audioEngine == nil else { return }

        let engine = AVAudioEngine()
        let node = AVAudioSourceNode(format: playbackFormat) {
            [weak self] _, _, frameCount, bufferList -> OSStatus in
            guard let self else { return noErr }
            let frames = Int(frameCount)
            let ablp = UnsafeMutableAudioBufferListPointer(bufferList)

            // Wait for prefill before outputting audio
            let avail = self.ringAvailable()
            if !self.prefilled {
                if avail < self.prefillThreshold {
                    for ch in 0..<ablp.count {
                        if let dst = ablp[ch].mData?.assumingMemoryBound(to: Float.self) {
                            memset(dst, 0, frames * MemoryLayout<Float>.size)
                        }
                    }
                    return noErr
                }
                self.prefilled = true
            }

            // Read mono samples from ring buffer
            let tmp = UnsafeMutablePointer<Float>.allocate(capacity: frames)
            defer { tmp.deallocate() }
            let got = self.ringRead(tmp, count: frames)

            // Mono -> stereo: copy to both L and R channels
            for ch in 0..<ablp.count {
                if let dst = ablp[ch].mData?.assumingMemoryBound(to: Float.self) {
                    if got > 0 {
                        memcpy(dst, tmp, got * MemoryLayout<Float>.size)
                    }
                    // Zero-fill remainder on underrun (no clicks)
                    if got < frames {
                        memset(dst.advanced(by: got), 0, (frames - got) * MemoryLayout<Float>.size)
                    }
                }
            }
            return noErr
        }

        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: playbackFormat)
        engine.mainMixerNode.outputVolume = volume

        do {
            try engine.start()
        } catch {
            print("[SDKAudioReceiver] AudioEngine start error: \(error)")
            return
        }

        self.audioEngine = engine
        self.sourceNode = node
    }

    private func stopAudioEngine() {
        if let engine = audioEngine, engine.isRunning {
            engine.stop()
        }
        if let node = sourceNode, let engine = audioEngine {
            engine.detach(node)
        }
        sourceNode = nil
        audioEngine = nil
    }

    // MARK: - iOS Audio Session

    private func configureAudioSession() {
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default,
                                    options: [.defaultToSpeaker, .mixWithOthers, .allowBluetooth])
            try session.setPreferredSampleRate(48000)
            try session.setActive(true)
        } catch {
            print("[SDKAudioReceiver] AudioSession error: \(error)")
        }
    }

    // MARK: - UDP Relay Connection

    private func connectRelay() {
        let host = "relay.solun.art"
        let port: UInt16 = 5100
        let ch = channel
        let deviceName = UIDevice.current.name

        recvQueue.async { [weak self] in
            guard let self else { return }

            // DNS resolve
            var hints = addrinfo()
            hints.ai_family = AF_INET
            hints.ai_socktype = SOCK_DGRAM
            var res: UnsafeMutablePointer<addrinfo>?
            guard getaddrinfo(host, "\(port)", &hints, &res) == 0, let addrInfo = res else {
                DispatchQueue.main.async { self.state = .error }
                return
            }
            memcpy(&self.relayAddr, addrInfo.pointee.ai_addr, Int(addrInfo.pointee.ai_addrlen))
            freeaddrinfo(res)

            // Create UDP socket
            self.udpSocket = Darwin.socket(AF_INET, SOCK_DGRAM, 0)
            guard self.udpSocket >= 0 else {
                DispatchQueue.main.async { self.state = .error }
                return
            }

            // 5 ms recv timeout for responsiveness
            var tv = timeval(tv_sec: 0, tv_usec: 5000)
            setsockopt(self.udpSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

            // Send HELLO x3 (100 ms apart)
            for i in 0..<3 {
                self.sendMessage("HELLO\n")
                if i < 2 { usleep(100_000) }
            }

            // JOIN:<channel>::<device_name>\n
            self.sendMessage("JOIN:\(ch)::\(deviceName)\n")

            self.running = true
            DispatchQueue.main.async {
                self.isConnected = true
                self.state = .connecting
            }

            // Start heartbeat timer (every 5 seconds)
            self.startHeartbeat(channel: ch, deviceName: deviceName)

            // Receive loop
            self.recvLoop()
        }
    }

    private func sendMessage(_ msg: String) {
        guard udpSocket >= 0 else { return }
        msg.withCString { ptr in
            withUnsafePointer(to: relayAddr) { addrPtr in
                let sa = UnsafeRawPointer(addrPtr).assumingMemoryBound(to: sockaddr.self)
                _ = Darwin.sendto(udpSocket, ptr, strlen(ptr), 0, sa,
                                  socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }

    private func startHeartbeat(channel: String, deviceName: String) {
        let timer = DispatchSource.makeTimerSource(queue: recvQueue)
        timer.schedule(deadline: .now() + 5, repeating: 5.0, leeway: .milliseconds(500))
        timer.setEventHandler { [weak self] in
            guard let self, self.running else { return }
            self.sendMessage("HELLO\n")
            self.sendMessage("JOIN:\(channel)::\(deviceName)\n")
        }
        timer.resume()
        heartbeatTimer = timer
    }

    // MARK: - Receive Loop

    private func recvLoop() {
        var buf = [UInt8](repeating: 0, count: 16384)
        var sender = sockaddr_in()
        var senderLen = socklen_t(MemoryLayout<sockaddr_in>.size)

        while running && udpSocket >= 0 {
            let n = withUnsafeMutablePointer(to: &sender) { senderPtr -> Int in
                senderPtr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    Darwin.recvfrom(udpSocket, &buf, buf.count, 0, sa, &senderLen)
                }
            }
            if n <= 0 { continue }

            // Minimum RTP header = 12 bytes
            guard n >= 12 else { continue }

            // RTP version check: (byte[0] & 0xC0) == 0x80
            guard (buf[0] & 0xC0) == 0x80 else { continue }

            handleOSTPPacket(buf, length: n)
        }
    }

    // MARK: - OSTP Packet Handling (simplified, matches MinApp)

    private func handleOSTPPacket(_ buf: [UInt8], length: Int) {
        let payloadType = buf[1] & 0x7F

        // Only handle S24 (PT=96) — same filter as working MinApp
        guard payloadType == 96 else { return }

        // Parse RTP extension header to find payload offset
        var off = 12
        if buf[0] & 0x10 != 0 && length >= 16 {
            off = 16 + (Int(buf[14]) << 8 | Int(buf[15])) * 4
        }

        // Strip CRC-32 trailer (last 4 bytes)
        let end = length - 4
        guard end > off else { return }

        // Decode S24: all samples straight into ring buffer (same as MinApp)
        let scale: Float = 1.0 / 8388608.0
        var i = off
        // Use stack buffer to avoid Array allocation per packet
        let maxSamples = (end - off) / 4
        let samples = UnsafeMutablePointer<Float>.allocate(capacity: maxSamples)
        defer { samples.deallocate() }
        var count = 0
        while i + 3 < end {
            let v = Int32(buf[i]) | (Int32(buf[i+1]) << 8) | (Int32(buf[i+2]) << 16) | (Int32(buf[i+3]) << 24)
            samples[count] = Float(v) * scale
            count += 1
            i += 4
        }

        writeSamples(samples, count: count)

        // Update stats on main thread (coalesce)
        let newCount = OSAtomicAdd64(1, &_packetsReceivedAtomic)
        if newCount % 50 == 0 || newCount == 1 {
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.packetsReceived = UInt64(self._packetsReceivedAtomic)
                if self.state != .receiving {
                    self.state = .receiving
                    self.isReceivingAudio = true
                }
            }
        }
    }

    private var _packetsReceivedAtomic: Int64 = 0

    // MARK: - ADPCM Decode (kept for compatibility but radio uses S24)

    private func decodeADPCM(_ buf: [UInt8], offset: Int, end: Int, channels: Int) {
        let payloadLen = end - offset
        guard payloadLen >= 4 else { return }

        // 4-byte ADPCM header: predictor (int16 LE), step index, reserved
        var predicted = Int16(bitPattern: UInt16(buf[offset]) | (UInt16(buf[offset + 1]) << 8))
        var stepIndex = min(buf[offset + 2], 88)

        let adpcmStart = offset + 4
        let adpcmLen = end - adpcmStart
        guard adpcmLen > 0 else { return }

        // Each byte = 2 nibbles = 2 samples. Downmix to mono (take first channel).
        let totalDecodedSamples = adpcmLen * 2
        let framesPerChannel = totalDecodedSamples / max(channels, 1)

        let samples = UnsafeMutablePointer<Float>.allocate(capacity: framesPerChannel)
        defer { samples.deallocate() }
        var outIdx = 0

        var sampleCount = 0
        for i in 0..<adpcmLen {
            let byte = buf[adpcmStart + i]
            for nibbleIdx in 0..<2 {
                let code: UInt8 = nibbleIdx == 0 ? (byte & 0x0F) : (byte >> 4)
                let step = Int32(Self.stepTable[Int(stepIndex)])

                var delta = step >> 3
                if code & 4 != 0 { delta += step }
                if code & 2 != 0 { delta += step >> 1 }
                if code & 1 != 0 { delta += step >> 2 }
                if code & 8 != 0 { delta = -delta }

                let newPredicted = max(-32768, min(32767, Int32(predicted) + delta))
                predicted = Int16(newPredicted)

                let newIdx = max(0, min(88, Int8(stepIndex) + Self.indexTable[Int(code)]))
                stepIndex = UInt8(newIdx)

                // Take every Nth sample for mono downmix (first channel only)
                if sampleCount % max(channels, 1) == 0 && outIdx < framesPerChannel {
                    samples[outIdx] = Float(predicted) / 32768.0
                    outIdx += 1
                }
                sampleCount += 1
            }
        }

        writeSamples(samples, count: outIdx)
    }
}
