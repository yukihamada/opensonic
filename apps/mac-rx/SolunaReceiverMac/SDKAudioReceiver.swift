//
//  SDKAudioReceiver.swift
//  SolunaReceiverMac
//
//  Pure-Swift audio receiver using the same architecture as SolunaSDK.
//  Drop-in replacement for AudioReceiver (C++ bridge).
//
//  Pipeline: UDP relay (BSD socket) -> OSTP parse -> S24/ADPCM decode
//            -> lock-free ring buffer -> AVAudioSourceNode pull playback
//

import Foundation
import AVFoundation
import Combine

// MARK: - SDKAudioReceiver

@MainActor
final class SDKAudioReceiver: ObservableObject {

    static let shared = SDKAudioReceiver()

    // MARK: - Published State (matches AudioReceiver interface)

    enum State: String {
        case stopped    = "Stopped"
        case connecting = "Connecting..."
        case receiving  = "Receiving"
        case error      = "Error"
    }

    @Published private(set) var state: State = .stopped
    @Published private(set) var packetsReceived: UInt64 = 0
    @Published private(set) var packetsDropped: UInt64 = 0
    @Published private(set) var isReceivingAudio = false
    @Published var volume: Float = 1.0 { didSet { engine?.mainMixerNode.outputVolume = isMuted ? 0 : volume } }
    @Published var isMuted: Bool = false { didSet { engine?.mainMixerNode.outputVolume = isMuted ? 0 : volume } }
    @Published var relayHost: String = "relay.solun.art"
    @Published var channel: String = "soluna"

    var isPlaying: Bool { state == .receiving || state == .connecting }

    // Stubs for properties ContentView references but SDK doesn't need yet
    @Published private(set) var errorMessage: String?
    @Published var bufferMs: UInt32 = 60
    @Published var isSyncMode: Bool = true
    @Published var syncDelayMs: UInt32 = 200
    @Published private(set) var activeOutputs: Set<UInt32> = []
    @Published private(set) var relayState: String = "disconnected"

    // MARK: - Audio Engine

    private var engine: AVAudioEngine?
    private var sourceNode: AVAudioSourceNode?
    private let playbackFormat: AVAudioFormat

    // MARK: - Ring Buffer (lock-free SPSC)

    /// Capacity in mono Float samples: 4 seconds @ 48 kHz mono = 192000
    private let ringCapacity = 192_000
    private var ringBuffer: UnsafeMutablePointer<Float>
    private var writePos: Int64 = 0
    private var readPos: Int64 = 0
    private let prefillThreshold = 4800  // 100 ms @ 48 kHz mono
    private var prefilled = false

    // MARK: - Relay Connection

    private var udpSocket: Int32 = -1
    private var relayAddr = sockaddr_in()
    private var recvThread: Thread?
    private var heartbeatTimer: Timer?
    private let running = SDKAtomicFlag()

    // MARK: - Stats

    private var statsTimer: Timer?
    private var watchdogTimer: Timer?
    private var lastPacketCount: UInt64 = 0
    private var staleTicks = 0

    // Packet counter updated from recv thread (atomic)
    private var _packetsReceivedAtomic: Int64 = 0

    // MARK: - Init

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
        running.set(false)
        if udpSocket >= 0 { Darwin.close(udpSocket); udpSocket = -1 }
        engine?.stop()
        ringBuffer.deallocate()
    }

    // MARK: - Public API

    func start() {
        guard state == .stopped || state == .error else { return }
        errorMessage = nil
        state = .connecting

        flushRing()
        startAudioEngine()
        connectRelay()
        startStatsPolling()
        startWatchdog()
    }

    func stop() {
        stopWatchdog()
        stopStatsPolling()
        disconnectRelay()
        stopAudioEngine()

        state = .stopped
        isReceivingAudio = false
        relayState = "disconnected"
    }

    func toggle() {
        if isPlaying { stop() } else { start() }
    }

    func setChannel(_ name: String) {
        let wasPlaying = isPlaying
        if wasPlaying { stop() }
        channel = name
        if wasPlaying { start() }
    }

    // MARK: - Audio Engine Setup

    private func startAudioEngine() {
        guard engine == nil else { return }

        let eng = AVAudioEngine()
        let fmt = playbackFormat

        let node = AVAudioSourceNode(format: fmt) { [weak self] _, _, frameCount, bufferList -> OSStatus in
            guard let self else { return noErr }
            let frames = Int(frameCount)
            let ablp = UnsafeMutableAudioBufferListPointer(bufferList)

            // Wait for prefill
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

            // Mono -> stereo: copy same data to both L and R channels
            for ch in 0..<ablp.count {
                if let dst = ablp[ch].mData?.assumingMemoryBound(to: Float.self) {
                    if got > 0 {
                        memcpy(dst, tmp, got * MemoryLayout<Float>.size)
                    }
                    if got < frames {
                        memset(dst.advanced(by: got), 0, (frames - got) * MemoryLayout<Float>.size)
                    }
                }
            }
            return noErr
        }

        eng.attach(node)
        eng.connect(node, to: eng.mainMixerNode, format: fmt)
        eng.mainMixerNode.outputVolume = isMuted ? 0 : volume

        do {
            try eng.start()
        } catch {
            print("[SDKAudioReceiver] Engine start error: \(error)")
            state = .error
            errorMessage = error.localizedDescription
            return
        }

        self.engine = eng
        self.sourceNode = node
    }

    private func stopAudioEngine() {
        if let node = sourceNode, let eng = engine {
            eng.stop()
            eng.detach(node)
        }
        sourceNode = nil
        engine = nil
        flushRing()
    }

    // MARK: - Ring Buffer Operations (lock-free)

    private func ringAvailable() -> Int {
        let w = OSAtomicAdd64(0, &writePos)
        let r = OSAtomicAdd64(0, &readPos)
        return Int(w - r)
    }

    /// Write mono float samples into ring buffer. Called from recv thread.
    private func ringWrite(_ samples: UnsafePointer<Float>, count: Int) {
        let w = Int(OSAtomicAdd64(0, &writePos))
        let cap = ringCapacity
        // Drop if ring is nearly full (leave 10% headroom)
        if ringAvailable() > cap - cap / 10 { return }
        for i in 0..<count {
            ringBuffer[(w + i) % cap] = samples[i]
        }
        OSAtomicAdd64(Int64(count), &writePos)
    }

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

    // MARK: - Relay Connection (BSD Socket)

    private func connectRelay() {
        guard !running.value else { return }

        let host = relayHost
        let ch = channel

        // All network setup on background thread (DNS + socket + usleep block main/audio)
        DispatchQueue.global(qos: .userInteractive).async { [weak self] in
            guard let self else { return }

            // Resolve relay IP
            var addr = sockaddr_in()
            addr.sin_family = sa_family_t(AF_INET)
            addr.sin_port = UInt16(5100).bigEndian
            inet_pton(AF_INET, "52.194.128.180", &addr.sin_addr)

            // Also try DNS (use IP as fallback)
            var hints = addrinfo()
            hints.ai_family = AF_INET
            hints.ai_socktype = SOCK_DGRAM
            var res: UnsafeMutablePointer<addrinfo>?
            if getaddrinfo(host, "5100", &hints, &res) == 0, let addrInfo = res {
                memcpy(&addr, addrInfo.pointee.ai_addr, Int(addrInfo.pointee.ai_addrlen))
                freeaddrinfo(res)
            }

            self.relayAddr = addr

            // Create UDP socket
            let sock = socket(AF_INET, SOCK_DGRAM, 0)
            guard sock >= 0 else { return }
            self.udpSocket = sock

            // Receive timeout: 5ms for low latency
            var tv = timeval(tv_sec: 0, tv_usec: 5000)
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

            // Send JOIN (like MinApp — just JOIN, no HELLO x3 with sleep)
            let deviceName = Host.current().localizedName ?? "SolunaSDK-Mac"
            let joinMsg = "JOIN:\(ch)::\(deviceName)\n"
            joinMsg.withCString { ptr in
                withUnsafePointer(to: &self.relayAddr) { addrPtr in
                    let sa = UnsafeRawPointer(addrPtr).assumingMemoryBound(to: sockaddr.self)
                    _ = sendto(sock, ptr, strlen(ptr), 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }

            self.running.set(true)

            DispatchQueue.main.async {
                self.relayState = "connected"
            }

            // Receive loop (stays on this background thread)
            self.recvLoop()
        }

        // Heartbeat every 5 seconds
        heartbeatTimer = Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
            guard let self, self.running.value, self.udpSocket >= 0 else { return }
            self.sendMessage("HELLO\n")
            let devName = Host.current().localizedName ?? "SolunaSDK-Mac"
            self.sendMessage("JOIN:\(self.channel)::\(devName)\n")
        }
    }

    private func disconnectRelay() {
        running.set(false)
        heartbeatTimer?.invalidate()
        heartbeatTimer = nil

        if udpSocket >= 0 {
            Darwin.close(udpSocket)
            udpSocket = -1
        }
    }

    private func sendMessage(_ message: String) {
        guard udpSocket >= 0 else { return }
        message.withCString { ptr in
            withUnsafePointer(to: relayAddr) { addrPtr in
                let sockaddrPtr = UnsafeRawPointer(addrPtr).assumingMemoryBound(to: sockaddr.self)
                _ = sendto(udpSocket, ptr, strlen(ptr), 0, sockaddrPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }

    // MARK: - Receive Loop (background thread)

    private func recvLoop() {
        var buf = [UInt8](repeating: 0, count: 16384)
        var sender = sockaddr_in()
        var senderLen = socklen_t(MemoryLayout<sockaddr_in>.size)

        while running.value && udpSocket >= 0 {
            let n = withUnsafeMutablePointer(to: &sender) { senderPtr -> Int in
                senderPtr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockaddrPtr in
                    recvfrom(udpSocket, &buf, buf.count, 0, sockaddrPtr, &senderLen)
                }
            }
            if n <= 0 { continue }
            if n < 12 { continue }  // Too small for RTP

            // RTP/OSTP audio packet: (byte[0] & 0xC0) == 0x80
            if (buf[0] & 0xC0) == 0x80 {
                handleAudioPacket(buf, length: n)
                OSAtomicIncrement64(&_packetsReceivedAtomic)
            }
            // Text control messages are ignored for now
        }
    }

    // MARK: - OSTP Packet Handling (simplified, matches MinApp)

    private func handleAudioPacket(_ buf: [UInt8], length: Int) {
        let payloadType = buf[1] & 0x7F

        // Only handle S24 (PT=96) — same filter as working MinApp
        guard payloadType == 96 else { return }

        // Parse RTP extension header to find payload offset
        var off = 12  // RTP header
        if buf[0] & 0x10 != 0 && length >= 16 {
            off = 16 + (Int(buf[14]) << 8 | Int(buf[15])) * 4
        }

        // Strip CRC-32 trailer (last 4 bytes)
        let end = length - 4
        guard end > off else { return }

        // Decode S24: all samples straight into ring buffer (same as MinApp)
        let scale: Float = 1.0 / 8388608.0
        var samples = [Float]()
        samples.reserveCapacity(96)
        var i = off
        while i + 3 < end {
            let v = Int32(buf[i]) | (Int32(buf[i+1]) << 8) | (Int32(buf[i+2]) << 16) | (Int32(buf[i+3]) << 24)
            samples.append(Float(v) * scale)
            i += 4
        }

        samples.withUnsafeBufferPointer { ptr in
            if let base = ptr.baseAddress {
                ringWrite(base, count: samples.count)
            }
        }

        // Signal receiving state (once)
        if !isReceivingAudio {
            DispatchQueue.main.async { [weak self] in
                self?.isReceivingAudio = true
                self?.state = .receiving
            }
        }
    }

    // MARK: - ADPCM Decode

    /// Decode IMA-ADPCM payload to mono float.
    private func decodeADPCM(_ payload: [UInt8], channels: Int) {
        guard payload.count >= 4 else { return }

        // 4-byte header: predictor (int16 LE), step index, reserved
        var predicted = Int16(bitPattern: UInt16(payload[0]) | (UInt16(payload[1]) << 8))
        var stepIndex = min(payload[2], 88)

        let adpcmData = Array(payload.dropFirst(4))
        let nSamples = adpcmData.count * 2
        guard nSamples > 0 else { return }

        let framesPerChannel = nSamples / max(channels, 1)

        // Decode ADPCM and take first channel only (mono downmix)
        var samples = [Float](repeating: 0, count: framesPerChannel)
        let scale: Float = 1.0 / 32768.0
        var sampleCount = 0
        var outIdx = 0

        for byte in adpcmData {
            for nibbleIdx in 0..<2 {
                let code: UInt8 = nibbleIdx == 0 ? (byte & 0x0F) : (byte >> 4)
                let step = Int32(SDKADPCMStepTable[Int(stepIndex)])

                var delta = step >> 3
                if code & 4 != 0 { delta += step }
                if code & 2 != 0 { delta += step >> 1 }
                if code & 1 != 0 { delta += step >> 2 }
                if code & 8 != 0 { delta = -delta }

                let newPredicted = max(-32768, min(32767, Int32(predicted) + delta))
                predicted = Int16(newPredicted)

                let newIdx = max(0, min(88, Int8(stepIndex) + SDKADPCMIndexTable[Int(code)]))
                stepIndex = UInt8(newIdx)

                // Take only first channel (mono)
                if sampleCount % max(channels, 1) == 0 && outIdx < framesPerChannel {
                    samples[outIdx] = Float(predicted) * scale
                    outIdx += 1
                }
                sampleCount += 1
            }
        }

        samples.withUnsafeBufferPointer { ptr in
            if let base = ptr.baseAddress {
                ringWrite(base, count: outIdx)
            }
        }
    }

    // MARK: - Stats Polling

    private func startStatsPolling() {
        statsTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                let count = UInt64(OSAtomicAdd64(0, &self._packetsReceivedAtomic))
                self.packetsReceived = count
            }
        }
    }

    private func stopStatsPolling() {
        statsTimer?.invalidate()
        statsTimer = nil
    }

    // MARK: - Watchdog

    private func startWatchdog() {
        lastPacketCount = packetsReceived
        staleTicks = 0
        watchdogTimer = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.watchdogTick() }
        }
    }

    private func stopWatchdog() {
        watchdogTimer?.invalidate()
        watchdogTimer = nil
        staleTicks = 0
    }

    private func watchdogTick() {
        guard isPlaying else { return }
        if packetsReceived == lastPacketCount {
            staleTicks += 1
            if staleTicks >= 10 {
                print("[SDKAudioReceiver] Watchdog: no packets for \(staleTicks * 3)s — reconnecting")
                stop()
                Task { @MainActor in
                    try? await Task.sleep(nanoseconds: 3_000_000_000)
                    self.start()
                }
            }
        } else {
            staleTicks = 0
            lastPacketCount = packetsReceived
        }
    }
}

// MARK: - Thread-safe Atomic Flag

private final class SDKAtomicFlag: @unchecked Sendable {
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

// MARK: - IMA-ADPCM Tables

/// Standard IMA-ADPCM step size table (89 entries).
private let SDKADPCMStepTable: [Int16] = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]

/// Standard IMA-ADPCM index adjustment table.
private let SDKADPCMIndexTable: [Int8] = [
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
]
