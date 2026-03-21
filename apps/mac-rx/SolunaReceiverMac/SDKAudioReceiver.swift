//
//  SDKAudioReceiver.swift
//  SolunaReceiverMac
//
//  Pure-Swift audio receiver for relay playback.
//  Pipeline: UDP relay -> OSTP/RTP parse -> S24-in-S32LE decode
//            -> lock-free SPSC ring buffer -> AVAudioSourceNode pull
//

import Foundation
import AVFoundation
import Combine

// MARK: - SDKAudioReceiver

@MainActor
final class SDKAudioReceiver: ObservableObject {

    static let shared = SDKAudioReceiver()

    // MARK: - Published State

    enum State: String {
        case stopped    = "Stopped"
        case connecting = "Connecting..."
        case receiving  = "Receiving"
        case error      = "Error"
    }

    @Published private(set) var state: State = .stopped
    @Published private(set) var packetsReceived: UInt64 = 0
    @Published private(set) var isReceivingAudio = false
    @Published var volume: Float = 1.0 { didSet { engine?.mainMixerNode.outputVolume = isMuted ? 0 : volume } }
    @Published var isMuted: Bool = false { didSet { engine?.mainMixerNode.outputVolume = isMuted ? 0 : volume } }
    @Published var relayHost: String = "relay.solun.art"
    @Published var channel: String = "soluna"

    var isPlaying: Bool { state == .receiving || state == .connecting }

    // Stubs referenced by ContentView
    @Published private(set) var errorMessage: String?
    @Published var bufferMs: UInt32 = 60
    @Published var isSyncMode: Bool = true
    @Published var syncDelayMs: UInt32 = 200
    @Published private(set) var activeOutputs: Set<UInt32> = []
    @Published private(set) var relayState: String = "disconnected"
    @Published private(set) var packetsDropped: UInt64 = 0
    @Published private(set) var bufferFillMs: Int = 0      // ring buffer fill in ms
    @Published private(set) var packetsPerSec: Int = 0     // recv rate

    // MARK: - Audio Engine

    private var engine: AVAudioEngine?
    private var sourceNode: AVAudioSourceNode?
    private let playbackFormat: AVAudioFormat

    // MARK: - Ring Buffer (lock-free SPSC, 4s @ 48 kHz mono)

    private let ringCapacity = 192_000
    private let ringBuffer: UnsafeMutablePointer<Float>
    private var writePos: Int64 = 0
    private var readPos: Int64 = 0
    private let prefillThreshold = 4800  // 100 ms

    // Pre-allocated scratch buffer for audio callback (avoid malloc on RT thread)
    private let scratchBuffer: UnsafeMutablePointer<Float>
    private let scratchCapacity = 4096

    // MARK: - Relay Connection

    private var udpSocket: Int32 = -1
    private var relayAddr = sockaddr_in()
    private let running = SDKAtomicFlag()
    private let firstPacketReceived = SDKAtomicFlag()

    // Pre-allocated decode buffer for recv loop (avoid per-packet alloc)
    private let decodeBuffer: UnsafeMutablePointer<Float>
    private let decodeCapacity = 256

    // MARK: - Stats

    private var statsTimer: Timer?
    private var watchdogTimer: Timer?
    private var lastPacketCount: UInt64 = 0
    private var staleTicks = 0
    private var _packetsReceivedAtomic: Int64 = 0

    // Heartbeat timer (DispatchSource on recv queue for thread safety)
    private var heartbeatSource: DispatchSourceTimer?
    private let recvQueue = DispatchQueue(label: "com.soluna.sdkrecv", qos: .userInteractive)

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
        scratchBuffer = .allocate(capacity: scratchCapacity)
        decodeBuffer = .allocate(capacity: decodeCapacity)
    }

    // MARK: - Public API

    func start() {
        guard state == .stopped || state == .error else { return }
        errorMessage = nil
        state = .connecting
        _packetsReceivedAtomic = 0
        firstPacketReceived.set(false)

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

    // MARK: - Audio Engine (AVAudioSourceNode pull-based)

    private func startAudioEngine() {
        guard engine == nil else { return }

        let eng = AVAudioEngine()
        let fmt = playbackFormat
        let scratch = scratchBuffer
        let scratchCap = scratchCapacity

        let node = AVAudioSourceNode(format: fmt) { [weak self] _, _, frameCount, bufferList -> OSStatus in
            guard let self else { return noErr }
            let frames = Int(frameCount)
            let ablp = UnsafeMutableAudioBufferListPointer(bufferList)

            // Prefill gate: check via ring positions (no separate flag needed)
            let avail = self.ringAvailable()
            if avail < self.prefillThreshold && !self.firstPacketReceived.value {
                for ch in 0..<ablp.count {
                    if let dst = ablp[ch].mData?.assumingMemoryBound(to: Float.self) {
                        memset(dst, 0, frames * MemoryLayout<Float>.size)
                    }
                }
                return noErr
            }

            // Read mono samples using pre-allocated scratch buffer
            let readCount = min(frames, scratchCap)
            let got = self.ringRead(scratch, count: readCount)

            // Mono -> stereo: copy to both L and R channels
            for ch in 0..<ablp.count {
                if let dst = ablp[ch].mData?.assumingMemoryBound(to: Float.self) {
                    if got > 0 { memcpy(dst, scratch, got * MemoryLayout<Float>.size) }
                    if got < frames { memset(dst.advanced(by: got), 0, (frames - got) * MemoryLayout<Float>.size) }
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
    }

    // MARK: - Ring Buffer (lock-free SPSC)

    private func ringAvailable() -> Int {
        let w = OSAtomicAdd64(0, &writePos)
        let r = OSAtomicAdd64(0, &readPos)
        return min(Int(w - r), ringCapacity)  // clamp to prevent out-of-bounds
    }

    private func ringWrite(_ samples: UnsafePointer<Float>, count: Int) {
        let w = Int(OSAtomicAdd64(0, &writePos))
        let cap = ringCapacity
        if ringAvailable() > cap * 9 / 10 { return }  // drop if nearly full
        for i in 0..<count { ringBuffer[(w + i) % cap] = samples[i] }
        OSAtomicAdd64(Int64(count), &writePos)
    }

    private func ringRead(_ dst: UnsafeMutablePointer<Float>, count: Int) -> Int {
        let n = min(ringAvailable(), count)
        let r = Int(OSAtomicAdd64(0, &readPos))
        let cap = ringCapacity
        for i in 0..<n { dst[i] = ringBuffer[(r + i) % cap] }
        OSAtomicAdd64(Int64(n), &readPos)
        return n
    }

    private func flushRing() {
        // Safe: only called when both audio engine and recv loop are stopped
        writePos = 0
        readPos = 0
    }

    // MARK: - Relay Connection

    private func connectRelay() {
        guard !running.value else { return }

        let host = relayHost
        let ch = channel

        recvQueue.async { [weak self] in
            guard let self else { return }

            // DNS resolve
            var hints = addrinfo()
            hints.ai_family = AF_INET
            hints.ai_socktype = SOCK_DGRAM
            var res: UnsafeMutablePointer<addrinfo>?
            guard getaddrinfo(host, "5100", &hints, &res) == 0, let addrInfo = res else {
                DispatchQueue.main.async {
                    self.state = .error
                    self.errorMessage = "DNS resolution failed for \(host)"
                }
                return
            }
            var addr = sockaddr_in()
            memcpy(&addr, addrInfo.pointee.ai_addr, Int(addrInfo.pointee.ai_addrlen))
            freeaddrinfo(res)
            self.relayAddr = addr

            // Create UDP socket
            let sock = socket(AF_INET, SOCK_DGRAM, 0)
            guard sock >= 0 else { return }
            self.udpSocket = sock

            // 5ms recv timeout
            var tv = timeval(tv_sec: 0, tv_usec: 5000)
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

            // JOIN
            let deviceName = Host.current().localizedName ?? "SolunaSDK-Mac"
            self.sendUDP("JOIN:\(ch)::\(deviceName)\n", sock: sock, addr: &self.relayAddr)

            self.running.set(true)
            DispatchQueue.main.async { self.relayState = "connected" }

            // Heartbeat on recv queue (thread-safe: same queue owns socket)
            let hb = DispatchSource.makeTimerSource(queue: self.recvQueue)
            hb.schedule(deadline: .now() + 5, repeating: 5.0)
            hb.setEventHandler { [weak self] in
                guard let self, self.running.value, self.udpSocket >= 0 else { return }
                self.sendUDP("HELLO\n", sock: self.udpSocket, addr: &self.relayAddr)
                self.sendUDP("JOIN:\(ch)::\(deviceName)\n", sock: self.udpSocket, addr: &self.relayAddr)
            }
            hb.resume()
            self.heartbeatSource = hb

            // Receive loop
            self.recvLoop()
        }
    }

    private func disconnectRelay() {
        running.set(false)
        heartbeatSource?.cancel()
        heartbeatSource = nil

        if udpSocket >= 0 {
            Darwin.close(udpSocket)
            udpSocket = -1
        }
    }

    private func sendUDP(_ message: String, sock: Int32, addr: UnsafeMutablePointer<sockaddr_in>) {
        message.withCString { ptr in
            addr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                _ = sendto(sock, ptr, strlen(ptr), 0, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }

    // MARK: - Receive Loop

    private func recvLoop() {
        var buf = [UInt8](repeating: 0, count: 4096)
        var sender = sockaddr_in()
        var senderLen = socklen_t(MemoryLayout<sockaddr_in>.size)
        let scale: Float = 1.0 / 8388608.0  // 2^23 for S24-in-S32LE
        let decodeBuf = decodeBuffer
        let decodeCap = decodeCapacity

        while running.value && udpSocket >= 0 {
            let n = withUnsafeMutablePointer(to: &sender) { sp -> Int in
                sp.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    recvfrom(udpSocket, &buf, buf.count, 0, sa, &senderLen)
                }
            }
            guard n > 12 else { continue }
            guard (buf[0] & 0xC0) == 0x80 else { continue }  // RTP version=2
            guard (buf[1] & 0x7F) == 96 else { continue }    // PT=96 (S24)

            // Parse extension header offset
            var off = 12
            if buf[0] & 0x10 != 0 && n >= 16 {
                let extLen = (Int(buf[14]) << 8 | Int(buf[15])) * 4
                off = 16 + extLen
                guard off < n else { continue }  // malformed extension
            }

            // Strip CRC-32 trailer
            let end = n - 4
            guard end > off else { continue }

            // Decode S24-in-S32LE to mono float (pre-allocated buffer)
            var count = 0
            var i = off
            while i + 3 < end && count < decodeCap {
                let v = Int32(buf[i]) | (Int32(buf[i+1]) << 8) | (Int32(buf[i+2]) << 16) | (Int32(buf[i+3]) << 24)
                decodeBuf[count] = Float(v) * scale
                count += 1
                i += 4
            }

            ringWrite(decodeBuf, count: count)
            OSAtomicIncrement64(&_packetsReceivedAtomic)

            // Signal first packet (atomic, safe from any thread)
            if !firstPacketReceived.value {
                firstPacketReceived.set(true)
                DispatchQueue.main.async { [weak self] in
                    self?.isReceivingAudio = true
                    self?.state = .receiving
                }
            }
        }
    }

    // MARK: - Stats & Watchdog

    private var _lastPktCount: Int64 = 0

    private func startStatsPolling() {
        statsTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                let current = OSAtomicAdd64(0, &self._packetsReceivedAtomic)
                self.packetsPerSec = Int(current - self._lastPktCount)
                self._lastPktCount = current
                self.packetsReceived = UInt64(current)
                self.bufferFillMs = self.ringAvailable() * 1000 / 48000  // samples → ms
            }
        }
    }

    private func stopStatsPolling() {
        statsTimer?.invalidate()
        statsTimer = nil
    }

    private func startWatchdog() {
        lastPacketCount = 0
        staleTicks = 0
        watchdogTimer = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.watchdogTick() }
        }
    }

    private func stopWatchdog() {
        watchdogTimer?.invalidate()
        watchdogTimer = nil
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
