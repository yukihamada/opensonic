//
//  SDKAudioReceiver.swift
//  SolunaReceiver (iOS)
//
//  Pure-Swift relay audio receiver using AVAudioSourceNode.
//  Pipeline: UDP recv -> S24-in-S32LE decode -> SPSC ring buffer -> AVAudioSourceNode
//

import Foundation
import AVFoundation
import UIKit
import Darwin
import CoreMotion

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
    @Published private(set) var bufferFillMs: Int = 0
    @Published private(set) var packetsPerSec: Int = 0
    @Published private(set) var syncOffsetMs: Double = 0
    @Published private(set) var outputLatencyMs: Double = 0  // output device latency (incl. Bluetooth)
    @Published var channel: String = "soluna" {
        didSet { targetTotalLatencyMs = Self.latencyForChannel(channel) }
    }

    // Bluetooth latency compensation: target total latency so all devices sync
    // Live channels use lower latency for real-time feel; radio uses higher for stability
    var targetTotalLatencyMs: Double = 300

    /// Per-channel config fetched from relay server
    static var channelConfigs: [String: [String: Any]] = [:]
    static var configLoaded = false

    /// Fetch channel config from relay server (called once on first use)
    static func loadChannelConfig() {
        guard !configLoaded else { return }
        configLoaded = true
        guard let url = URL(string: "https://relay.solun.art/api/channel-config") else { return }
        URLSession.shared.dataTask(with: url) { data, _, _ in
            guard let data, let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let channels = json["channels"] as? [String: [String: Any]] else { return }
            DispatchQueue.main.async { channelConfigs = channels }
        }.resume()
    }

    /// Determine target latency based on channel config from server
    static func latencyForChannel(_ ch: String) -> Double {
        // Server config takes priority
        if let config = channelConfigs[ch], let ms = config["latencyMs"] as? Double { return ms }
        if let config = channelConfigs[ch], let ms = config["latencyMs"] as? Int { return Double(ms) }
        // Fallback: prefix-based detection
        let liveChannels: Set<String> = ["live", "stage", "dj", "karaoke", "talk"]
        if liveChannels.contains(ch) || ch.hasPrefix("live-") { return 50 }
        return 300
    }
    private var outputLatencyFrames: Int = 0  // latency in samples at 48kHz
    @Published var volume: Float = 1.0 {
        didSet { audioEngine?.mainMixerNode.outputVolume = volume }
    }

    /// Spatial audio toggle (persisted via UserDefaults "spatialAudioEnabled")
    @Published var spatialAudioEnabled: Bool = UserDefaults.standard.bool(forKey: "spatialAudioEnabled") {
        didSet {
            UserDefaults.standard.set(spatialAudioEnabled, forKey: "spatialAudioEnabled")
            if spatialAudioEnabled { startHeadTracking() } else { stopHeadTracking() }
        }
    }

    var isPlaying: Bool { state == .receiving || state == .connecting }

    // MARK: - Crossfade

    enum FadePhase {
        case none
        case fadingOut
        case fadingIn
    }

    private var fadePhase: FadePhase = .none
    private var fadeProgress: Float = 0       // 0.0 to 1.0
    private var pendingChannel: String?
    private let fadeDurationSamples: Int = 48000  // 1 second at 48kHz

    /// The current fade multiplier for the audio callback (thread-safe read)
    private var _fadeMultiplier: Float = 1.0

    // MARK: - Audio Engine

    private var audioEngine: AVAudioEngine?
    private var sourceNode: AVAudioSourceNode?
    private var environmentNode: AVAudioEnvironmentNode?
    private let playbackFormat: AVAudioFormat
    private var headphoneMotionManager: CMHeadphoneMotionManager?

    // MARK: - Ring Buffer (lock-free SPSC, 4s @ 48 kHz mono)

    private let ringCapacity = 192_000
    private let ringBuffer: UnsafeMutablePointer<Float>
    private var writePos: Int64 = 0
    private var readPos: Int64 = 0
    private let basePrefillThreshold = 14400  // 300 ms — enough for WAN jitter

    // Pre-allocated scratch buffer for audio callback (no malloc on RT thread)
    private let scratchBuffer: UnsafeMutablePointer<Float>
    private let scratchCapacity = 4096

    // MARK: - Network

    private var udpSocket: Int32 = -1
    private var relayAddr = sockaddr_in()
    private let recvQueue = DispatchQueue(label: "com.soluna.sdkrecv", qos: .userInteractive)
    private let running = SDKAtomicFlag()
    private let firstPacketReceived = SDKAtomicFlag()
    private var heartbeatTimer: DispatchSourceTimer?
    private var _packetsReceivedAtomic: Int64 = 0

    // Pre-allocated decode buffer for recv loop
    private let decodeBuffer: UnsafeMutablePointer<Float>
    private let decodeCapacity = 256

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
        scratchBuffer = .allocate(capacity: scratchCapacity)
        decodeBuffer = .allocate(capacity: decodeCapacity)
    }

    deinit {
        ringBuffer.deallocate()
        scratchBuffer.deallocate()
        decodeBuffer.deallocate()
    }

    // MARK: - Public API

    func start(channel: String? = nil) {
        guard state == .stopped || state == .error else { return }
        Self.loadChannelConfig()  // Fetch server config on first start
        if let ch = channel { self.channel = ch }

        state = .connecting
        packetsReceived = 0
        _packetsReceivedAtomic = 0
        isReceivingAudio = false
        isConnected = false
        firstPacketReceived.set(false)

        configureAudioSession()
        UIApplication.shared.isIdleTimerDisabled = true

        flushRing()
        startAudioEngine()
        connectRelay()
    }

    func stop() {
        running.set(false)

        heartbeatTimer?.cancel()
        heartbeatTimer = nil

        if udpSocket >= 0 {
            Darwin.close(udpSocket)
            udpSocket = -1
        }

        stopAudioEngine()
        ClockSync.shared.reset()

        state = .stopped
        isConnected = false
        isReceivingAudio = false
        syncOffsetMs = 0

        UIApplication.shared.isIdleTimerDisabled = false
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    func setChannel(_ name: String) {
        guard name != channel else { return }
        if isPlaying {
            crossfadeTo(channel: name)
        } else {
            channel = name
        }
    }

    /// Crossfade: fade out current audio, switch relay, fade in new audio.
    /// Uses a volume envelope applied in the audio callback.
    func crossfadeTo(channel newChannel: String) {
        guard fadePhase == .none else {
            // Already crossfading; queue the channel
            pendingChannel = newChannel
            return
        }
        pendingChannel = newChannel
        fadePhase = .fadingOut
        fadeProgress = 0
    }

    /// Called from recvLoop context when fade-out completes.
    /// Disconnects old relay, flushes ring, connects to new channel.
    private func completeFadeOutAndSwitch() {
        guard let newChannel = pendingChannel else {
            fadePhase = .none
            return
        }

        // Stop the old relay connection (socket + heartbeat) without stopping audio engine
        running.set(false)
        heartbeatTimer?.cancel()
        heartbeatTimer = nil
        if udpSocket >= 0 {
            Darwin.close(udpSocket)
            udpSocket = -1
        }

        // Flush ring buffer for clean start
        flushRing()
        firstPacketReceived.set(false)
        ClockSync.shared.reset()

        // Update channel
        DispatchQueue.main.async { [weak self] in
            self?.channel = newChannel
        }

        // Begin fade-in phase
        fadePhase = .fadingIn
        fadeProgress = 0
        pendingChannel = nil

        // Connect to new relay channel
        let host = "relay.solun.art"
        let ch = newChannel
        let deviceName = UIDevice.current.name

        recvQueue.async { [weak self] in
            guard let self else { return }

            // DNS resolve
            var hints = addrinfo()
            hints.ai_family = AF_INET
            hints.ai_socktype = SOCK_DGRAM
            var res: UnsafeMutablePointer<addrinfo>?
            guard getaddrinfo(host, "5100", &hints, &res) == 0, let addrInfo = res else {
                DispatchQueue.main.async { self.state = .error }
                return
            }
            memcpy(&self.relayAddr, addrInfo.pointee.ai_addr, Int(addrInfo.pointee.ai_addrlen))
            freeaddrinfo(res)

            self.udpSocket = Darwin.socket(AF_INET, SOCK_DGRAM, 0)
            guard self.udpSocket >= 0 else {
                DispatchQueue.main.async { self.state = .error }
                return
            }

            var tv = timeval(tv_sec: 0, tv_usec: 50000)  // 50ms recv timeout for WAN
            setsockopt(self.udpSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

            self.sendUDP("JOIN:\(ch)::\(deviceName)\n")
            self.running.set(true)

            DispatchQueue.main.async {
                self.isConnected = true
                self.state = .connecting
            }

            let timer = DispatchSource.makeTimerSource(queue: self.recvQueue)
            timer.schedule(deadline: .now() + 5, repeating: 5.0)
            timer.setEventHandler { [weak self] in
                guard let self, self.running.value else { return }
                self.sendUDP("HELLO\n")
            }
            timer.resume()
            self.heartbeatTimer = timer

            self.recvLoop()
        }
    }

    // MARK: - Ring Buffer (lock-free SPSC)

    private func ringAvailable() -> Int {
        let w = OSAtomicAdd64(0, &writePos)
        let r = OSAtomicAdd64(0, &readPos)
        return min(Int(w - r), ringCapacity)  // clamp
    }

    private func writeSamples(_ samples: UnsafePointer<Float>, count: Int) {
        let w = Int(OSAtomicAdd64(0, &writePos))
        let cap = ringCapacity
        if ringAvailable() > cap * 9 / 10 { return }
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
        writePos = 0
        readPos = 0
    }

    // MARK: - Audio Engine (AVAudioSourceNode)

    private func startAudioEngine() {
        guard audioEngine == nil else { return }

        let engine = AVAudioEngine()
        let scratch = scratchBuffer
        let scratchCap = scratchCapacity

        let node = AVAudioSourceNode(format: playbackFormat) {
            [weak self] _, _, frameCount, bufferList -> OSStatus in
            guard let self else { return noErr }
            let frames = Int(frameCount)
            let ablp = UnsafeMutableAudioBufferListPointer(bufferList)

            // Prefill gate — dynamic threshold includes BT latency compensation
            let avail = self.ringAvailable()
            let prefill = self.dynamicPrefillThreshold
            if avail < prefill {
                if avail == 0 && self.firstPacketReceived.value {
                    // Buffer ran dry — re-enter prefill mode to rebuild buffer
                    self.firstPacketReceived.set(false)
                }
                for ch in 0..<ablp.count {
                    if let dst = ablp[ch].mData?.assumingMemoryBound(to: Float.self) {
                        memset(dst, 0, frames * MemoryLayout<Float>.size)
                    }
                }
                return noErr
            }

            // Read mono, duplicate to stereo
            let readCount = min(frames, scratchCap)
            let got = self.ringRead(scratch, count: readCount)

            // Apply crossfade envelope
            let phase = self.fadePhase
            if phase != .none && got > 0 {
                let fadeDur = Float(self.fadeDurationSamples)
                var progress = self.fadeProgress
                for i in 0..<got {
                    let t = min(progress / fadeDur, 1.0)
                    let multiplier: Float
                    switch phase {
                    case .fadingOut:
                        multiplier = 1.0 - t  // 1.0 -> 0.0
                    case .fadingIn:
                        multiplier = t         // 0.0 -> 1.0
                    case .none:
                        multiplier = 1.0
                    }
                    scratch[i] *= multiplier
                    progress += 1.0
                }
                self.fadeProgress = progress
                self._fadeMultiplier = (phase == .fadingOut) ? max(0, 1.0 - progress / fadeDur) : min(1.0, progress / fadeDur)

                // Check if fade phase completed
                if progress >= fadeDur {
                    if phase == .fadingOut {
                        // Trigger channel switch on recvQueue
                        DispatchQueue.main.async { [weak self] in
                            self?.completeFadeOutAndSwitch()
                        }
                    } else if phase == .fadingIn {
                        self.fadePhase = .none
                        self.fadeProgress = 0
                        self._fadeMultiplier = 1.0
                    }
                }
            }

            for ch in 0..<ablp.count {
                if let dst = ablp[ch].mData?.assumingMemoryBound(to: Float.self) {
                    if got > 0 { memcpy(dst, scratch, got * MemoryLayout<Float>.size) }
                    if got < frames { memset(dst.advanced(by: got), 0, (frames - got) * MemoryLayout<Float>.size) }
                }
            }
            return noErr
        }

        engine.attach(node)

        // Spatial audio: insert AVAudioEnvironmentNode when enabled
        if spatialAudioEnabled {
            let envNode = AVAudioEnvironmentNode()
            engine.attach(envNode)
            // Source positioned 2m in front of the listener
            envNode.listenerPosition = AVAudio3DPoint(x: 0, y: 0, z: 0)
            // Mono format for the source node → environment node connection
            let monoFormat = AVAudioFormat(
                commonFormat: .pcmFormatFloat32,
                sampleRate: 48000,
                channels: 1,
                interleaved: false
            )!
            engine.connect(node, to: envNode, format: monoFormat)
            engine.connect(envNode, to: engine.mainMixerNode, format: playbackFormat)
            self.environmentNode = envNode
            startHeadTracking()
        } else {
            engine.connect(node, to: engine.mainMixerNode, format: playbackFormat)
            self.environmentNode = nil
        }

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
        stopHeadTracking()
        if let engine = audioEngine, engine.isRunning { engine.stop() }
        if let node = sourceNode, let engine = audioEngine { engine.detach(node) }
        if let envNode = environmentNode, let engine = audioEngine { engine.detach(envNode) }
        sourceNode = nil
        environmentNode = nil
        audioEngine = nil
    }

    // MARK: - iOS Audio Session

    private func configureAudioSession() {
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default,
                                    options: [.defaultToSpeaker, .mixWithOthers, .allowBluetooth])
            try session.setPreferredSampleRate(48000)
            // Enable spatial audio / multichannel content rendering on compatible headphones
            if #available(iOS 15.0, *) {
                try session.setSupportsMultichannelContent(true)
            }
            try session.setActive(true)
        } catch {
            print("[SDKAudioReceiver] AudioSession error: \(error)")
        }
    }

    // MARK: - Head Tracking (Spatial Audio with AirPods Pro)

    private func startHeadTracking() {
        guard spatialAudioEnabled else { return }
        guard headphoneMotionManager == nil else { return }

        let manager = CMHeadphoneMotionManager()
        guard manager.isDeviceMotionAvailable else {
            print("[SDKAudioReceiver] Head tracking not available on this device")
            return
        }
        headphoneMotionManager = manager
        manager.startDeviceMotionUpdates(to: .main) { [weak self] motion, error in
            guard let self, let motion, let envNode = self.environmentNode else { return }
            // Map head rotation to listener orientation
            let yaw = Float(motion.attitude.yaw)
            let pitch = Float(motion.attitude.pitch)
            envNode.listenerAngularOrientation = AVAudio3DAngularOrientation(
                yaw: yaw * 180.0 / .pi,
                pitch: pitch * 180.0 / .pi,
                roll: 0
            )
        }
        print("[SDKAudioReceiver] Head tracking started")
    }

    private func stopHeadTracking() {
        headphoneMotionManager?.stopDeviceMotionUpdates()
        headphoneMotionManager = nil
    }

    // MARK: - Output Latency Measurement (Bluetooth compensation)

    /// Measure the current output device latency in milliseconds.
    /// On iOS, AVAudioSession.outputLatency includes Bluetooth codec delay.
    private func getOutputLatencyMs() -> Double {
        let session = AVAudioSession.sharedInstance()
        let outputLatency = session.outputLatency       // seconds (includes BT)
        let ioBufferDuration = session.ioBufferDuration  // seconds
        return (outputLatency + ioBufferDuration) * 1000.0
    }

    /// Update output latency measurement and recalculate compensation frames.
    private func updateOutputLatency() {
        let ms = getOutputLatencyMs()
        outputLatencyMs = ms
        outputLatencyFrames = Int(ms * 48.0)  // 48kHz → frames
    }

    /// Dynamic prefill threshold: base prefill + extra buffer to compensate for low-latency devices.
    /// All devices target the same total latency (targetTotalLatencyMs).
    /// High-latency devices (Bluetooth) get less extra buffer; low-latency devices (wired) get more.
    /// Whether the output device latency exceeds the target (BT can't meet sync)
    @Published private(set) var latencyExceeded: Bool = false

    private var dynamicPrefillThreshold: Int {
        let gap = targetTotalLatencyMs - outputLatencyMs
        if gap < 0 {
            // BT latency exceeds target — can't sync, minimize prefill for lowest achievable latency
            latencyExceeded = true
            return Int(max(20, targetTotalLatencyMs) * 48.0)  // minimum buffer = target or 20ms
        }
        latencyExceeded = false
        let extraFrames = Int(gap * 48.0)
        return basePrefillThreshold + extraFrames
    }

    // MARK: - UDP Relay Connection

    private func connectRelay() {
        let host = "relay.solun.art"
        let ch = channel
        let deviceName = UIDevice.current.name

        recvQueue.async { [weak self] in
            guard let self else { return }

            // DNS resolve
            var hints = addrinfo()
            hints.ai_family = AF_INET
            hints.ai_socktype = SOCK_DGRAM
            var res: UnsafeMutablePointer<addrinfo>?
            guard getaddrinfo(host, "5100", &hints, &res) == 0, let addrInfo = res else {
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

            // 50ms recv timeout for WAN
            var tv = timeval(tv_sec: 0, tv_usec: 50000)  // 50ms recv timeout for WAN
            setsockopt(self.udpSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

            // JOIN
            self.sendUDP("JOIN:\(ch)::\(deviceName)\n")

            self.running.set(true)
            DispatchQueue.main.async {
                self.isConnected = true
                self.state = .connecting
            }

            // Heartbeat on recv queue (thread-safe)
            let timer = DispatchSource.makeTimerSource(queue: self.recvQueue)
            timer.schedule(deadline: .now() + 5, repeating: 5.0)
            timer.setEventHandler { [weak self] in
                guard let self, self.running.value else { return }
                self.sendUDP("HELLO\n")
            }
            timer.resume()
            self.heartbeatTimer = timer

            // Receive loop
            self.recvLoop()
        }
    }

    /// Send raw bytes over the relay UDP socket (used by SoundTeleport).
    func sendUDPBytes(_ data: [UInt8]) {
        guard udpSocket >= 0 else { return }
        data.withUnsafeBufferPointer { buf in
            guard let base = buf.baseAddress else { return }
            withUnsafePointer(to: relayAddr) { addrPtr in
                let sa = UnsafeRawPointer(addrPtr).assumingMemoryBound(to: sockaddr.self)
                _ = Darwin.sendto(udpSocket, base, data.count, 0, sa,
                                  socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }

    /// Send a raw string over the relay UDP socket (used by ChatManager).
    func sendUDP(_ msg: String) {
        guard udpSocket >= 0 else { return }
        msg.withCString { ptr in
            withUnsafePointer(to: relayAddr) { addrPtr in
                let sa = UnsafeRawPointer(addrPtr).assumingMemoryBound(to: sockaddr.self)
                _ = Darwin.sendto(udpSocket, ptr, strlen(ptr), 0, sa,
                                  socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }

    // MARK: - Receive Loop

    private func recvLoop() {
        var buf = [UInt8](repeating: 0, count: 4096)
        var sender = sockaddr_in()
        var senderLen = socklen_t(MemoryLayout<sockaddr_in>.size)
        let scale: Float = 1.0 / 8388608.0
        let decodeBuf = decodeBuffer
        let decodeCap = decodeCapacity

        while running.value && udpSocket >= 0 {
            let n = withUnsafeMutablePointer(to: &sender) { sp -> Int in
                sp.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                    Darwin.recvfrom(udpSocket, &buf, buf.count, 0, sa, &senderLen)
                }
            }
            guard n > 0 else { continue }

            // TEXT messages are plain text (not RTP). Forward to ChatManager / ReactionManager.
            if buf[0] != 0x80, n > 5,
               let str = String(bytes: buf[0..<n], encoding: .utf8),
               str.hasPrefix("TEXT:") {
                if str.hasPrefix("TEXT:chat") {
                    DispatchQueue.main.async {
                        ChatManager.shared.handleRelayMessage(str)
                    }
                } else if str.hasPrefix("TEXT:react") {
                    DispatchQueue.main.async {
                        ReactionManager.shared.handleRelay(str)
                    }
                } else if str.hasPrefix("TEXT:request") || str.hasPrefix("TEXT:vote") {
                    DispatchQueue.main.async {
                        SongRequestManager.shared.handleRelay(str)
                    }
                } else if str.hasPrefix("TEXT:heartbeat") {
                    DispatchQueue.main.async {
                        HeartbeatManager.shared.handleRelay(str)
                    }
                }
                continue
            }

            guard n > 12 else { continue }
            guard (buf[0] & 0xC0) == 0x80 else { continue }  // RTP version=2
            guard (buf[1] & 0x7F) == 96 else { continue }    // PT=96 (S24)

            // Extract RTP timestamp (bytes 4-7, big-endian uint32)
            let rtpTs = UInt32(buf[4]) << 24 | UInt32(buf[5]) << 16 | UInt32(buf[6]) << 8 | UInt32(buf[7])

            // Set clock reference on first packet
            if !ClockSync.shared.hasReference {
                ClockSync.shared.setReference(rtpTimestamp: rtpTs, wallClockNs: ClockSync.shared.wallClockNs)
            }

            // Extension header offset
            var off = 12
            if buf[0] & 0x10 != 0 && n >= 16 {
                let extLen = (Int(buf[14]) << 8 | Int(buf[15])) * 4
                off = 16 + extLen
                guard off < n else { continue }
            }

            // Strip CRC trailer
            let end = n - 4
            guard end > off else { continue }

            // Decode S24-in-S32LE (pre-allocated buffer)
            var count = 0
            var i = off
            while i + 3 < end && count < decodeCap {
                let v = Int32(buf[i]) | (Int32(buf[i+1]) << 8) | (Int32(buf[i+2]) << 16) | (Int32(buf[i+3]) << 24)
                decodeBuf[count] = Float(v) * scale
                count += 1
                i += 4
            }

            writeSamples(decodeBuf, count: count)

            let pktCount = OSAtomicIncrement64(&_packetsReceivedAtomic)

            if !firstPacketReceived.value {
                firstPacketReceived.set(true)
                DispatchQueue.main.async { [weak self] in
                    self?.state = .receiving
                    self?.isReceivingAudio = true
                }
            }

            if pktCount % 100 == 0 {
                let bufMs = self.ringAvailable() * 1000 / 48000
                let offsetMs = ClockSync.shared.offsetMs(currentRtpTimestamp: rtpTs)
                DispatchQueue.main.async { [weak self] in
                    self?.packetsReceived = UInt64(pktCount)
                    self?.bufferFillMs = bufMs
                    self?.packetsPerSec = 500  // ~500 pkt/s at 48kHz/96samples
                    self?.syncOffsetMs = offsetMs
                    self?.updateOutputLatency()
                }
            }
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
