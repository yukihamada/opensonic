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
import CoreAudio
import Combine
import Accelerate

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
    @Published var channel: String = "soluna" {
        didSet { targetTotalLatencyMs = Self.latencyForChannel(channel) }
    }

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
    @Published private(set) var syncOffsetMs: Double = 0   // NTP sync offset
    @Published private(set) var outputLatencyMs: Double = 0  // output device latency (incl. Bluetooth)
    @Published private(set) var outputLevelL: Float = 0  // L channel peak (0-1)
    @Published private(set) var outputLevelR: Float = 0  // R channel peak (0-1)
    @Published private(set) var spectrumBands: [Float] = Array(repeating: 0, count: 32)

    // FFT infrastructure (pre-allocated for audio thread)
    private let fftSize = 2048
    private var fftSetup: OpaquePointer?  // vDSP_create_fftsetup result
    private let fftLog2n = vDSP_Length(11) // log2(2048)
    private var fftWindowBuffer = [Float](repeating: 0, count: 2048)

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
        if let config = channelConfigs[ch], let ms = config["latencyMs"] as? Double { return ms }
        if let config = channelConfigs[ch], let ms = config["latencyMs"] as? Int { return Double(ms) }
        let liveChannels: Set<String> = ["live", "stage", "dj", "karaoke", "talk"]
        if liveChannels.contains(ch) || ch.hasPrefix("live-") { return 50 }
        return 300
    }
    private var outputLatencyFrames: Int = 0  // latency in samples at 48kHz
    private var _prefillPtr: UnsafeMutablePointer<Int64>?  // atomic prefill for RT thread

    // MARK: - Audio Engine

    private var engine: AVAudioEngine?
    private var sourceNode: AVAudioSourceNode?
    private let playbackFormat: AVAudioFormat

    // MARK: - Ring Buffer (lock-free SPSC, 4s @ 48 kHz mono)

    private let ringCapacity = 192_000
    private let ringBuffer: UnsafeMutablePointer<Float>
    private var writePos: Int64 = 0
    private var readPos: Int64 = 0
    private let basePrefillThreshold = 14400  // 300 ms — enough for WAN jitter

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
    private var _lastRtpTimestamp: UInt32 = 0

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

        // FFT setup (created once, reused every frame)
        fftSetup = vDSP_create_fftsetup(fftLog2n, FFTRadix(kFFTRadix2))
        vDSP_hann_window(&fftWindowBuffer, vDSP_Length(fftSize), Int32(vDSP_HANN_NORM))

        // Auto-start after init
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { [weak self] in
            guard let self, self.state == .stopped else { return }
            let ch = UserDefaults.standard.string(forKey: "channel") ?? "soluna"
            self.channel = ch
            self.relayHost = "relay.solun.art"
            self.start()
        }
    }

    // MARK: - Public API

    private var lastStartTime: Date = .distantPast

    func start() {
        guard state == .stopped || state == .error else { return }
        Self.loadChannelConfig()  // Fetch server config on first start
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
        ClockSync.shared.reset()

        state = .stopped
        isReceivingAudio = false
        relayState = "disconnected"
        syncOffsetMs = 0
    }

    func toggle() {
        if isPlaying { stop() } else { start() }
    }

    func setChannel(_ name: String) {
        guard name != channel else { return }  // Skip if already on this channel
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

        // Capture raw pointers for RT audio callback (no weak self — avoid GC stalls)
        let ringBuf = ringBuffer
        let ringCap = ringCapacity
        var rPos = UnsafeMutablePointer<Int64>.allocate(capacity: 1)
        rPos.initialize(to: 0)
        let wPosPtr = UnsafeMutablePointer<Int64>.allocate(capacity: 1)
        // We'll use the instance's writePos/readPos directly via pointer
        let wpAddr = withUnsafeMutablePointer(to: &writePos) { $0 }
        let rpAddr = withUnsafeMutablePointer(to: &readPos) { $0 }
        // Dynamic prefill pointer: updated by stats polling with BT latency compensation
        let prefillPtr = UnsafeMutablePointer<Int64>.allocate(capacity: 1)
        prefillPtr.initialize(to: Int64(basePrefillThreshold))
        self._prefillPtr = prefillPtr
        let fpReceived = firstPacketReceived

        let node = AVAudioSourceNode(format: fmt) { _, _, frameCount, bufferList -> OSStatus in
            let frames = Int(frameCount)
            let ablp = UnsafeMutableAudioBufferListPointer(bufferList)

            let w = OSAtomicAdd64(0, wpAddr)
            let r = OSAtomicAdd64(0, rpAddr)
            let avail = min(Int(w - r), ringCap)

            let prefill = Int(OSAtomicAdd64(0, prefillPtr))
            if avail < prefill {
                if avail == 0 && fpReceived.value {
                    // Buffer ran dry — re-enter prefill mode to rebuild buffer
                    fpReceived.set(false)
                }
                for c in 0..<ablp.count {
                    if let d = ablp[c].mData?.assumingMemoryBound(to: Float.self) { memset(d, 0, frames * 4) }
                }
                return noErr
            }

            let n = min(avail, min(frames, scratchCap))
            let rr = Int(OSAtomicAdd64(0, rpAddr))
            for i in 0..<n { scratch[i] = ringBuf[(rr + i) % ringCap] }
            OSAtomicAdd64(Int64(n), rpAddr)

            for c in 0..<ablp.count {
                if let d = ablp[c].mData?.assumingMemoryBound(to: Float.self) {
                    if n > 0 { memcpy(d, scratch, n * 4) }
                    if n < frames { memset(d.advanced(by: n), 0, (frames - n) * 4) }
                }
            }
            return noErr
        }

        eng.attach(node)
        eng.connect(node, to: eng.mainMixerNode, format: fmt)
        eng.mainMixerNode.outputVolume = isMuted ? 0 : volume

        // Install tap on output for level metering + FFT spectrum analysis
        let mixerFormat = eng.mainMixerNode.outputFormat(forBus: 0)
        let capturedFFTSize = fftSize
        let capturedLog2n = fftLog2n
        let capturedFFTSetup = fftSetup
        let capturedWindow = fftWindowBuffer
        let capturedSampleRate = Float(mixerFormat.sampleRate > 0 ? mixerFormat.sampleRate : 48000)
        eng.mainMixerNode.installTap(onBus: 0, bufferSize: 2048, format: mixerFormat) { [weak self] buffer, _ in
            guard let self else { return }
            guard let channelData = buffer.floatChannelData else { return }
            let frameCount = Int(buffer.frameLength)

            // Level metering
            var peakL: Float = 0
            var peakR: Float = 0
            let ptrL = channelData[0]
            for i in 0..<frameCount { peakL = max(peakL, abs(ptrL[i])) }
            if buffer.format.channelCount >= 2 {
                let ptrR = channelData[1]
                for i in 0..<frameCount { peakR = max(peakR, abs(ptrR[i])) }
            } else {
                peakR = peakL
            }

            // FFT spectrum analysis
            let bands = SDKAudioReceiver.computeSpectrum(
                channelData[0], frameCount: frameCount,
                fftSize: capturedFFTSize, log2n: capturedLog2n,
                fftSetup: capturedFFTSetup, window: capturedWindow,
                sampleRate: capturedSampleRate
            )

            DispatchQueue.main.async {
                self.outputLevelL = peakL
                self.outputLevelR = peakR
                self.spectrumBands = bands
            }
        }

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
        if let eng = engine {
            eng.mainMixerNode.removeTap(onBus: 0)
            eng.stop()
            if let node = sourceNode { eng.detach(node) }
        }
        sourceNode = nil
        engine = nil
        outputLevelL = 0
        outputLevelR = 0
        spectrumBands = Array(repeating: 0, count: 32)
    }

    // MARK: - FFT Spectrum Computation (static, safe to call from audio thread)

    private static func computeSpectrum(
        _ samples: UnsafePointer<Float>, frameCount: Int,
        fftSize: Int, log2n: vDSP_Length,
        fftSetup: OpaquePointer?, window: [Float],
        sampleRate: Float
    ) -> [Float] {
        guard let fftSetup else { return Array(repeating: 0, count: 32) }

        let n = min(frameCount, fftSize)
        let halfN = fftSize / 2

        // Apply Hann window to input samples
        var windowed = [Float](repeating: 0, count: fftSize)
        for i in 0..<n { windowed[i] = samples[i] * window[i] }

        // Pack interleaved real data into split complex form
        var realp = [Float](repeating: 0, count: halfN)
        var imagp = [Float](repeating: 0, count: halfN)

        windowed.withUnsafeBufferPointer { buf in
            buf.baseAddress!.withMemoryRebound(to: DSPComplex.self, capacity: halfN) { complex in
                var split = DSPSplitComplex(realp: &realp, imagp: &imagp)
                vDSP_ctoz(complex, 2, &split, 1, vDSP_Length(halfN))
            }
        }

        // Forward FFT (in-place)
        var splitComplex = DSPSplitComplex(realp: &realp, imagp: &imagp)
        vDSP_fft_zrip(fftSetup, &splitComplex, 1, log2n, FFTDirection(kFFTDirection_Forward))

        // Compute squared magnitudes
        var magnitudes = [Float](repeating: 0, count: halfN)
        vDSP_zvmags(&splitComplex, 1, &magnitudes, 1, vDSP_Length(halfN))

        // Convert to dB scale (10 * log10)
        var one: Float = 1.0
        vDSP_vdbcon(magnitudes, 1, &one, &magnitudes, 1, vDSP_Length(halfN), 0)

        // Map FFT bins to 32 logarithmic display bands (20Hz – 20kHz)
        let binResolution = sampleRate / Float(fftSize)
        let minFreq: Float = 20
        let maxFreq: Float = 20000
        let logMin = log10(minFreq)
        let logMax = log10(maxFreq)

        var bands = [Float](repeating: 0, count: 32)
        for i in 0..<32 {
            let freqLow  = pow(10, logMin + (logMax - logMin) * Float(i) / 32)
            let freqHigh = pow(10, logMin + (logMax - logMin) * Float(i + 1) / 32)
            let binLow  = max(1, Int(freqLow / binResolution))
            let binHigh = min(halfN - 1, Int(freqHigh / binResolution))

            if binHigh >= binLow {
                var maxVal: Float = -120
                for b in binLow...binHigh {
                    maxVal = max(maxVal, magnitudes[b])
                }
                // Normalize: -60dB..0dB → 0.0..1.0
                bands[i] = max(0, min(1, (maxVal + 60) / 60))
            }
        }

        return bands
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
        print("[SDKRecv] connectRelay ENTER, channel=\(channel), host=\(relayHost)")
        // Force cleanup before connecting
        running.set(false)
        heartbeatSource?.cancel()
        heartbeatSource = nil
        if udpSocket >= 0 { Darwin.close(udpSocket); udpSocket = -1 }

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

            // 50ms recv timeout for WAN
            var tv = timeval(tv_sec: 0, tv_usec: 50000)
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

            // Extract RTP timestamp (bytes 4-7, big-endian uint32)
            let rtpTs = UInt32(buf[4]) << 24 | UInt32(buf[5]) << 16 | UInt32(buf[6]) << 8 | UInt32(buf[7])

            // Set clock reference on first packet
            if !ClockSync.shared.hasReference {
                ClockSync.shared.setReference(rtpTimestamp: rtpTs, wallClockNs: ClockSync.shared.wallClockNs)
            }
            self._lastRtpTimestamp = rtpTs

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

    // MARK: - Output Latency Measurement (Bluetooth compensation)

    /// Measure the current output device latency in milliseconds.
    /// Includes device latency + safety offset, covering Bluetooth codec delay.
    private func getOutputLatencyMs() -> Double {
        guard let eng = engine else { return 0 }
        let device = eng.outputNode.auAudioUnit.deviceID

        // Device latency (frames)
        var latency: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyLatency,
            mScope: kAudioObjectPropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        AudioObjectGetPropertyData(device, &address, 0, nil, &size, &latency)

        // Safety offset (frames)
        var safetyOffset: UInt32 = 0
        var soSize = UInt32(MemoryLayout<UInt32>.size)
        var soAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertySafetyOffset,
            mScope: kAudioObjectPropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        AudioObjectGetPropertyData(device, &soAddress, 0, nil, &soSize, &safetyOffset)

        // Convert frames to ms (assume 48kHz sample rate)
        return Double(latency + safetyOffset) / 48.0
    }

    /// Update output latency measurement and recalculate compensation frames.
    /// Also updates the atomic prefill pointer read by the RT audio callback.
    private func updateOutputLatency() {
        let ms = getOutputLatencyMs()
        outputLatencyMs = ms
        outputLatencyFrames = Int(ms * 48.0)  // 48kHz → frames
        // Update the dynamic prefill threshold atomically for the RT thread
        if let ptr = _prefillPtr {
            let newPrefill = Int64(dynamicPrefillThreshold)
            OSAtomicCompareAndSwap64(OSAtomicAdd64(0, ptr), newPrefill, ptr)
        }
    }

    /// Whether the output device latency exceeds the target (BT can't meet sync)
    @Published private(set) var latencyExceeded: Bool = false

    /// Dynamic prefill threshold with BT latency compensation.
    private var dynamicPrefillThreshold: Int {
        let gap = targetTotalLatencyMs - outputLatencyMs
        if gap < 0 {
            // BT latency exceeds target — can't sync, minimize prefill
            latencyExceeded = true
            return Int(max(20, targetTotalLatencyMs) * 48.0)
        }
        latencyExceeded = false
        let extraFrames = Int(gap * 48.0)
        return basePrefillThreshold + extraFrames
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
                self.syncOffsetMs = ClockSync.shared.offsetMs(currentRtpTimestamp: self._lastRtpTimestamp)
                self.updateOutputLatency()
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
                print("[SDKAudioReceiver] Watchdog: no packets for \(staleTicks * 3)s — sending re-JOIN")
                // Don't stop/restart (creates new socket flood) — just re-send JOIN on existing socket
                let ch = channel
                let deviceName = Host.current().localizedName ?? "SolunaSDK-Mac"
                recvQueue.async { [weak self] in
                    guard let self, self.udpSocket >= 0 else { return }
                    self.sendUDP("JOIN:\(ch)::\(deviceName)\n", sock: self.udpSocket, addr: &self.relayAddr)
                }
                staleTicks = 0  // Reset to avoid repeated re-JOINs
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
