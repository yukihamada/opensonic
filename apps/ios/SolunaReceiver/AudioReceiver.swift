//
//  AudioReceiver.swift
//  SolunaReceiver
//
//  Swift wrapper for the Objective-C++ audio receiver bridge
//

import Foundation
import Combine
import AVFoundation
import UIKit
import Network

/// Observable wrapper for SolunaAudioReceiver
@MainActor
final class AudioReceiver: ObservableObject {

    /// Connection state
    enum State: String {
        case stopped = "Stopped"
        case connecting = "Connecting..."
        case receiving = "Receiving"
        case error = "Error"

        init(from objcState: SolunaReceiverState) {
            switch objcState {
            case .stopped:
                self = .stopped
            case .connecting:
                self = .connecting
            case .receiving:
                self = .receiving
            case .error:
                self = .error
            @unknown default:
                self = .stopped
            }
        }
    }

    /// Current connection state
    @Published private(set) var state: State = .stopped

    /// Number of packets received
    @Published private(set) var packetsReceived: UInt64 = 0

    /// Number of packets dropped
    @Published private(set) var packetsDropped: UInt64 = 0

    /// Number of packets concealed by PLC
    @Published private(set) var packetsConcealed: UInt64 = 0

    /// Device health (good / stressed / silenced based on underrun rate)
    @Published private(set) var deviceHealth: SolunaDeviceHealth = .good

    /// Current volume (0.0 - 1.0)
    @Published var volume: Float = 1.0 {
        didSet {
            if !isMuted {
                receiver.volume = volume
            }
        }
    }

    /// Mute state — preserves volume level
    @Published var isMuted: Bool = false {
        didSet {
            receiver.volume = isMuted ? 0 : volume
        }
    }

    /// Jitter buffer target in ms (5–2000 ms, auto-adaptive from 60 ms)
    @Published var bufferMs: UInt32 = 60 {
        didSet {
            receiver.bufferTargetMs = bufferMs
        }
    }

    /// Synchronized playback mode — all receivers output at same wall-clock time
    /// Default: true (perfectly synced playback across devices)
    /// When toggled off (Jam/Fast mode), buffer is minimized for lowest latency
    @Published var isSyncMode: Bool = true {
        didSet {
            receiver.syncMode = isSyncMode
            if !isSyncMode {
                // Jam mode: minimize buffer for lowest possible latency
                bufferMs = 5
            } else {
                // Sync mode: restore comfortable buffer for sync accuracy
                bufferMs = 60
            }
        }
    }

    /// Target end-to-end delay in ms (50-1000) for sync mode
    @Published var syncDelayMs: UInt32 = 200 {
        didSet { receiver.syncDelayMs = syncDelayMs }
    }

    /// Whether mic transmit is active
    @Published private(set) var isMicTransmitting: Bool = false

    /// TX packets sent
    @Published private(set) var txPacketsSent: UInt64 = 0

    /// Mic input level (0.0 - 1.0) for UI meter
    @Published private(set) var micInputLevel: Float = 0.0

    /// Output audio peak level (0.0 - 1.0) for visualization
    @Published private(set) var outputLevel: Float = 0.0

    /// Push-to-Talk mode
    @Published var isPTTMode: Bool = false

    /// Network quality stats
    @Published var networkLatencyMs: Float = 0
    @Published var jitterMs: Float = 0
    @Published var packetLossPercent: Float = 0

    /// Now Playing metadata
    @Published var nowPlayingTitle: String?
    @Published var nowPlayingArtist: String?
    @Published var nowPlayingArtwork: URL?

    /// Error message if any
    @Published private(set) var errorMessage: String?

    /// Debug log from C++ bridge (on-screen diagnostics)
    @Published private(set) var debugLog: String = ""

    // ── WAN Relay ────────────────────────────────────────────────────────
    enum RelayState: String {
        case disconnected = "Disconnected"
        case connecting = "Connecting..."
        case connected = "Connected"
        case error = "Error"

        init(from objc: SolunaRelayState) {
            switch objc {
            case .disconnected: self = .disconnected
            case .connecting:   self = .connecting
            case .connected:    self = .connected
            case .error:        self = .error
            @unknown default:   self = .disconnected
            }
        }
    }

    @Published private(set) var relayState: RelayState = .disconnected
    @Published private(set) var relayGroup: String?
    @Published private(set) var relayError: String?

    /// Unique device ID (UUID v4), persisted across launches
    var deviceId: String {
        let key = "soluna_device_id"
        if let existing = UserDefaults.standard.string(forKey: key), !existing.isEmpty {
            return existing
        }
        let newId = UUID().uuidString.lowercased()
        UserDefaults.standard.set(newId, forKey: key)
        return newId
    }

    /// Reset device ID (re-generate)
    func resetDeviceId() -> String {
        let newId = UUID().uuidString.lowercased()
        UserDefaults.standard.set(newId, forKey: "soluna_device_id")
        return newId
    }

    /// Multicast group address
    var multicastGroup: String {
        get { receiver.multicastGroup }
        set { receiver.multicastGroup = newValue }
    }

    /// RTP port
    var port: UInt16 {
        get { receiver.port }
        set { receiver.port = newValue }
    }

    /// Number of channels
    var channels: UInt32 {
        get { receiver.channels }
        set { receiver.channels = newValue }
    }

    /// Whether currently playing
    var isPlaying: Bool {
        state == .receiving || state == .connecting
    }

    private let receiver: SolunaAudioReceiver
    private let delegateHandler: DelegateHandler
    private let networkMonitor = NWPathMonitor()
    private var wasPlayingBeforeDisconnect = false
    private var suppressInterruption = false
    private var interruptionObserver: Any?
    private var routeChangeObserver: Any?
    private var watchdogTimer: Timer?
    private var statsTimer: Timer?
    private var levelTimer: Timer?
    private var lastPacketCount: UInt64 = 0
    private var staleTicks: Int = 0

    init() {
        receiver = SolunaAudioReceiver.sharedInstance()
        delegateHandler = DelegateHandler()
        delegateHandler.audioReceiver = self
        receiver.delegate = delegateHandler
        setupNetworkMonitor()
        setupAudioInterruptionHandler()
        setupRouteChangeHandler()
        updateOutputLatency()
    }

    /// Start receiving audio — instant multicast + parallel peer scan.
    /// Audio begins immediately via multicast/relay. Peer discovery runs
    /// concurrently and switches to P2P only if a peer is found.
    func start() {
        guard state == .stopped || state == .error else { return }
        errorMessage = nil
        state = .connecting   // visual feedback

        // Configure audio session for reliable background playback
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default, options: [.duckOthers])
            try session.setPreferredIOBufferDuration(0.005) // 5ms — low latency with 1ch mono
            try session.setActive(true)
        } catch {
            print("[AudioReceiver] AVAudioSession error: \(error)")
        }

        // Prevent screen lock during playback
        UIApplication.shared.isIdleTimerDisabled = true

        // Apply preferStereo setting: if enabled, ensure at least 2 output channels
        let preferStereo = UserDefaults.standard.bool(forKey: "preferStereo")
        if preferStereo && receiver.channels < 2 {
            receiver.channels = 2
        }

        // Start audio output immediately — no waiting for peer scan
        let ok = receiver.start()
        if !ok { return }

        let ch = UserDefaults.standard.string(forKey: "channel") ?? "soluna"

        // Start watchdog, stats, meta callback, and audio fingerprinting
        startWatchdog()
        startStatsPolling()
        setupMetaCallback()
        setupFingerprintTap(channel: ch)

        // Connect to relay — use manual host if set, else auto-discover
        let manualHost = UserDefaults.standard.string(forKey: "relayHost") ?? ""
        if !manualHost.isEmpty {
            connectRelay(group: ch, host: manualHost, port: 5099)
        } else {
            connectRelay(group: ch)
            // WAN relay: just stay connected. Don't disconnect/retry —
            // the relay is the source of truth for remote channels.
            // The watchdog (30s no-packet timeout) handles real failures.
        }

        // P2P peer scan only when using manual/LAN host.
        // When connected to WAN relay, stay on relay — don't let LAN
        // peer discovery override the remote stream.
        if !manualHost.isEmpty {
            Task {
                let foundPeer = await PeerRelayManager.shared.scanForPeers(channel: ch)
                if foundPeer {
                    print("[AudioReceiver] Switched to P2P relay from peer")
                } else {
                    try? await Task.sleep(nanoseconds: 2_000_000_000)
                    guard state == .receiving else { return }
                    PeerRelayManager.shared.becomeDirectRelay()
                }
            }
        }
    }

    /// Listen for META messages from the WAN relay and update nowPlaying
    private func setupMetaCallback() {
        receiver.setMetaCallback { [weak self] jsonStr in
            guard let self, let data = jsonStr.data(using: .utf8),
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return }
            self.nowPlayingTitle = json["title"] as? String ?? json["track"] as? String
            self.nowPlayingArtist = json["artist"] as? String
            if let urlStr = json["artwork_url"] as? String {
                self.nowPlayingArtwork = URL(string: urlStr)
            }
        }

        // FILE: callback — download music file for file-sync mode
        receiver.setFileCallback { [weak self] filename in
            guard let self else { return }
            let enabled = UserDefaults.standard.bool(forKey: "fileSyncEnabled")
            // fileSyncEnabled defaults to true (key absent → false, so check registration)
            let isOn = UserDefaults.standard.object(forKey: "fileSyncEnabled") == nil ? true : enabled
            if !isOn { return }
            self.downloadAndPrepare(filename: filename)
        }

        // SYNC: callback — play/pause/seek in file-sync mode
        receiver.setSyncCallback { [weak self] syncCmd in
            guard let self else { return }
            let enabled = UserDefaults.standard.bool(forKey: "fileSyncEnabled")
            let isOn = UserDefaults.standard.object(forKey: "fileSyncEnabled") == nil ? true : enabled
            if !isOn { return }
            self.handleSyncCommand(syncCmd)
        }
    }

    // MARK: - File Sync Mode

    private var currentSyncFile: String?
    private var pendingSyncCmd: String?
    private var fileSyncTimer: DispatchSourceTimer?
    private var fileSyncAudioFile: AVAudioFile?
    private var fileSyncConverter: AVAudioConverter?
    private var fileSyncOutputFormat: AVAudioFormat?
    private var fileSyncPlaying: Bool = false
    private let fileSyncQueue = DispatchQueue(label: "com.soluna.filesync", qos: .userInteractive)
    private static let kPipelineSampleRate: Double = 48000

    private func downloadAndPrepare(filename: String) {
        let encoded = filename.addingPercentEncoding(withAllowedCharacters: .alphanumerics.union(CharacterSet(charactersIn: "-._~"))) ?? filename
        guard let url = URL(string: "http://relay.solun.art:5102/api/music/\(encoded)") else { return }

        // New track: stop current file-sync pump
        stopFileSyncPump()
        fileSyncQueue.sync {
            fileSyncAudioFile = nil
        }
        pendingSyncCmd = nil
        receiver.filesyncNetworkDisabled = false

        currentSyncFile = filename
        nowPlayingTitle = filename

        // Check if already cached
        let cacheDir = FileManager.default.temporaryDirectory.appendingPathComponent("soluna-music")
        try? FileManager.default.createDirectory(at: cacheDir, withIntermediateDirectories: true)
        let localFile = cacheDir.appendingPathComponent(filename)

        if FileManager.default.fileExists(atPath: localFile.path) {
            prepareAudioFile(url: localFile)
            receiver.sendReady(filename)
            return
        }

        // Download
        URLSession.shared.downloadTask(with: url) { [weak self] tempURL, response, error in
            guard let self, let tempURL else { return }
            let httpCode = (response as? HTTPURLResponse)?.statusCode ?? 0
            guard httpCode == 200 else { return }
            try? FileManager.default.moveItem(at: tempURL, to: localFile)
            DispatchQueue.main.async {
                self.prepareAudioFile(url: localFile)
                self.receiver.sendReady(filename)
            }
        }.resume()
    }

    private func prepareAudioFile(url: URL) {
        do {
            let audioFile = try AVAudioFile(forReading: url)
            let srcFmt = audioFile.processingFormat
            let dstChannels = UInt32(receiver.channels)
            guard let dstFmt = AVAudioFormat(standardFormatWithSampleRate: Self.kPipelineSampleRate,
                                              channels: dstChannels) else { return }
            let converter: AVAudioConverter?
            if srcFmt.sampleRate != Self.kPipelineSampleRate || srcFmt.channelCount != dstChannels {
                converter = AVAudioConverter(from: srcFmt, to: dstFmt)
            } else {
                converter = nil
            }

            fileSyncQueue.sync {
                fileSyncAudioFile = audioFile
                fileSyncConverter = converter
                fileSyncOutputFormat = dstFmt
            }

            if let pending = pendingSyncCmd {
                pendingSyncCmd = nil
                handleSyncCommand(pending)
            }
        } catch { }
    }

    private func startFileSyncPump() {
        stopFileSyncPump()
        var audioFile: AVAudioFile?
        var dstFmt: AVAudioFormat?
        fileSyncQueue.sync {
            audioFile = fileSyncAudioFile
            dstFmt = fileSyncOutputFormat
        }
        guard let audioFile, let dstFmt else { return }

        let outFramesPerPump = AVAudioFrameCount(Self.kPipelineSampleRate * 0.01)
        let dstChannels = dstFmt.channelCount

        fileSyncQueue.sync { fileSyncPlaying = true }
        // Flush stale PCM data before prefilling with file-sync audio
        receiver.flushRingBuffer()
        receiver.filesyncNetworkDisabled = true

        // Prefill 200ms
        for _ in 0..<20 {
            var af: AVAudioFile?
            fileSyncQueue.sync { af = fileSyncAudioFile }
            guard let af else { break }
            guard let buf = pumpOneChunk(audioFile: af, dstFmt: dstFmt, frameCount: outFramesPerPump, dstChannels: dstChannels) else { break }
            receiver.injectPcmSamples(buf.data, frameCount: UInt(buf.frames))
        }

        let timer = DispatchSource.makeTimerSource(queue: fileSyncQueue)
        timer.schedule(deadline: .now(), repeating: .milliseconds(10), leeway: .milliseconds(1))
        timer.setEventHandler { [weak self] in
            guard let self, self.fileSyncPlaying, let af = self.fileSyncAudioFile else { return }
            guard let buf = self.pumpOneChunk(audioFile: af, dstFmt: dstFmt, frameCount: outFramesPerPump, dstChannels: dstChannels) else {
                DispatchQueue.main.async {
                    self.stopFileSyncPump()
                    self.receiver.filesyncNetworkDisabled = false
                }
                return
            }
            self.receiver.injectPcmSamples(buf.data, frameCount: UInt(buf.frames))
        }
        timer.resume()
        fileSyncTimer = timer
    }

    private func pumpOneChunk(audioFile: AVAudioFile, dstFmt: AVAudioFormat, frameCount: AVAudioFrameCount, dstChannels: UInt32) -> (data: Data, frames: Int)? {
        let outBuf: AVAudioPCMBuffer

        if let converter = fileSyncConverter {
            guard let outputBuffer = AVAudioPCMBuffer(pcmFormat: dstFmt, frameCapacity: frameCount) else { return nil }
            var gotData = false
            var convError: NSError?
            let _ = converter.convert(to: outputBuffer, error: &convError) { inNumPackets, outStatus in
                guard let inputBuffer = AVAudioPCMBuffer(pcmFormat: audioFile.processingFormat, frameCapacity: inNumPackets) else {
                    outStatus.pointee = .noDataNow
                    return nil
                }
                do {
                    try audioFile.read(into: inputBuffer, frameCount: inNumPackets)
                    if inputBuffer.frameLength == 0 { outStatus.pointee = .endOfStream; return nil }
                    gotData = true
                    outStatus.pointee = .haveData
                    return inputBuffer
                } catch { outStatus.pointee = .endOfStream; return nil }
            }
            if let err = convError {
                NSLog("[FileSync] Converter error: %@", err.localizedDescription)
            }
            if !gotData && outputBuffer.frameLength == 0 { return nil }
            outBuf = outputBuffer
        } else {
            guard let directBuf = AVAudioPCMBuffer(pcmFormat: dstFmt, frameCapacity: frameCount) else { return nil }
            do {
                try audioFile.read(into: directBuf, frameCount: frameCount)
                if directBuf.frameLength == 0 { return nil }
            } catch { return nil }
            outBuf = directBuf
        }

        let frames = Int(outBuf.frameLength)
        let chCount = Int(dstChannels)
        var int32Buf = [Int32](repeating: 0, count: frames * chCount)
        for ch in 0..<chCount {
            guard let chData = outBuf.floatChannelData?[ch] else { continue }
            for i in 0..<frames {
                let sample = chData[i]
                let scaled = max(-1.0, min(1.0, sample)) * 8388608.0
                int32Buf[i * chCount + ch] = Int32(clamping: Int64(scaled))
            }
        }
        let data = Data(bytes: &int32Buf, count: int32Buf.count * MemoryLayout<Int32>.size)
        return (data, frames)
    }

    private func stopFileSyncPump() {
        fileSyncQueue.sync { fileSyncPlaying = false }
        fileSyncTimer?.cancel()
        fileSyncTimer = nil
    }

    private func handleSyncCommand(_ cmd: String) {
        var audioFile: AVAudioFile?
        fileSyncQueue.sync { audioFile = fileSyncAudioFile }
        guard let audioFile else {
            pendingSyncCmd = cmd
            return
        }

        let parts = cmd.split(separator: ":")
        guard let action = parts.first else { return }

        switch action {
        case "play":
            guard parts.count >= 3,
                  let posMs = Double(parts[1]),
                  let wallMs = Double(parts[2]) else { return }

            let nowMs = Date().timeIntervalSince1970 * 1000
            let elapsedMs = nowMs - wallMs
            // Add prefill offset: ring buffer prefilled with 200ms before playback starts
            let prefillMs: Double = 200.0
            let currentPosMs = posMs + max(0, elapsedMs) + prefillMs

            let srcRate = audioFile.processingFormat.sampleRate
            let durationMs = Double(audioFile.length) / srcRate * 1000.0
            if currentPosMs >= durationMs { return }

            fileSyncQueue.sync {
                let seekFrame = AVAudioFramePosition(currentPosMs / 1000.0 * srcRate)
                audioFile.framePosition = seekFrame
                fileSyncConverter?.reset()
            }
            startFileSyncPump()

        case "pause":
            stopFileSyncPump()
            receiver.filesyncNetworkDisabled = false

        case "seek":
            guard parts.count >= 2, let posMs = Double(parts[1]) else { return }
            fileSyncQueue.sync {
                let seekFrame = AVAudioFramePosition(posMs / 1000.0 * audioFile.processingFormat.sampleRate)
                audioFile.framePosition = seekFrame
                fileSyncConverter?.reset()
            }

        case "skip":
            stopFileSyncPump()
            fileSyncQueue.sync { fileSyncAudioFile = nil }
            receiver.filesyncNetworkDisabled = false

        default:
            break
        }
    }

    /// Stop receiving audio
    func stop() {
        // Stop mic TX if active
        if isMicTransmitting {
            receiver.stopMicTransmit()
            isMicTransmitting = false
        }

        teardownFingerprintTap()
        disconnectRelay()
        stopWatchdog()
        stopStatsPolling()
        receiver.setMetaCallback(nil)
        receiver.setFileCallback(nil)
        receiver.setSyncCallback(nil)
        stopFileSyncPump()
        fileSyncQueue.sync { fileSyncAudioFile = nil }
        currentSyncFile = nil
        pendingSyncCmd = nil
        receiver.filesyncNetworkDisabled = false
        if isDJActive { stopDJBroadcast() }
        nowPlayingTitle = nil
        nowPlayingArtist = nil
        nowPlayingArtwork = nil
        state = .stopped
        receiver.stop()
        PeerRelayManager.shared.stop()
        UIApplication.shared.isIdleTimerDisabled = false
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    /// Toggle microphone transmit on/off
    func toggleMic() {
        if isMicTransmitting {
            suppressInterruption = true
            receiver.stopMicTransmit()
            isMicTransmitting = false
            // Restore playback-only session
            do {
                let session = AVAudioSession.sharedInstance()
                try session.setCategory(.playback, mode: .default, options: [.duckOthers])
                try session.setActive(true)
            } catch {
                print("[AudioReceiver] Session restore error: \(error)")
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { [weak self] in
                self?.suppressInterruption = false
            }
        } else {
            AVAudioSession.sharedInstance().requestRecordPermission { [weak self] granted in
                Task { @MainActor in
                    guard let self, granted else { return }
                    // Switch to playAndRecord BEFORE starting mic
                    self.suppressInterruption = true
                    do {
                        let session = AVAudioSession.sharedInstance()
                        try session.setCategory(.playAndRecord, mode: .default,
                                                options: [.defaultToSpeaker, .allowBluetooth])
                        try session.setActive(true)
                    } catch {
                        print("[AudioReceiver] Session error: \(error)")
                        self.suppressInterruption = false
                        return
                    }
                    if self.receiver.startMicTransmit() {
                        self.isMicTransmitting = true
                    }
                    DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { [weak self] in
                        self?.suppressInterruption = false
                    }
                }
            }
        }
    }

    // MARK: - Talk Mode

    func setTalkMode(_ enabled: Bool) {
        receiver.setTalkMode(enabled)
    }

    // MARK: - DJ Broadcast

    /// Whether DJ broadcast is currently active
    @Published private(set) var isDJActive: Bool = false

    /// Current track filename (nil when not broadcasting)
    @Published private(set) var djCurrentTrack: String?

    /// Playback progress (0.0 – 1.0)
    @Published private(set) var djProgress: Float = 0.0

    /// Enable mic mixing while DJ is broadcasting
    @Published var djMicMixEnabled: Bool = false {
        didSet { receiver.djMicMixEnabled = djMicMixEnabled }
    }

    private var djPollTimer: Timer?

    /// Start DJ broadcast from a file URL (security-scoped resource).
    func startDJBroadcast(url: URL) {
        guard url.startAccessingSecurityScopedResource() else { return }
        let tmpURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("soluna-dj-\(url.lastPathComponent)")
        do {
            if FileManager.default.fileExists(atPath: tmpURL.path) {
                try FileManager.default.removeItem(at: tmpURL)
            }
            try FileManager.default.copyItem(at: url, to: tmpURL)
        } catch {
            url.stopAccessingSecurityScopedResource()
            return
        }
        url.stopAccessingSecurityScopedResource()

        guard receiver.startDJBroadcast(tmpURL.path) else { return }
        isDJActive = true
        djCurrentTrack = url.lastPathComponent
        startDJPollTimer()
    }

    /// Stop DJ broadcast.
    func stopDJBroadcast() {
        receiver.stopDJ()
        isDJActive = false
        djCurrentTrack = nil
        djProgress = 0
        stopDJPollTimer()
    }

    private func startDJPollTimer() {
        stopDJPollTimer()
        djPollTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                self.isDJActive = self.receiver.isDJActive
                self.djCurrentTrack = self.receiver.djCurrentTrack
                self.djProgress = self.receiver.djProgress
                if !self.isDJActive { self.stopDJPollTimer() }
            }
        }
    }

    private func stopDJPollTimer() {
        djPollTimer?.invalidate()
        djPollTimer = nil
    }

    /// Allow a specific device to use microphone (owner/DJ only)
    func allowMic(deviceId: String) {
        receiver.sendMicAllow(deviceId)
    }

    /// Deny a specific device from using microphone (owner/DJ only)
    func denyMic(deviceId: String) {
        receiver.sendMicDeny(deviceId)
    }

    /// Request mic list from relay
    func requestMicList() {
        receiver.requestMicList()
    }

    /// Request members list from relay
    func requestMembers() {
        receiver.requestMembers()
    }

    // MARK: - WAN Relay

    func connectRelay(group: String, password: String = "",
                      host: String = "relay.solun.art", port: UInt16 = 5100) {
        guard state == .connecting || state == .receiving else { return }
        let devId = deviceId
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }
            let ok = self.receiver.connect(toRelay: host, port: port,
                                            group: group, password: password,
                                            deviceId: devId)
            Task { @MainActor in
                self.updateRelayState()
                if !ok {
                    self.relayError = self.receiver.relayError ?? "Connection failed"
                }
            }
        }
    }

    /// Discover LAN solunad via Bonjour, connect there; else WAN relay
    private var lanBrowser: NWBrowser?
    private var lanResolver: ServiceResolver?

    func discoverAndConnectRelay(group: String) {
        // Use NWBrowser to find _soluna._tcp on LAN
        let browser = NWBrowser(for: .bonjour(type: "_soluna._tcp.", domain: "local."), using: .udp)
        lanBrowser = browser
        var done = false

        browser.browseResultsChangedHandler = { [weak self] results, _ in
            guard let self, !done else { return }
            for result in results {
                if case .service(let name, _, _, _) = result.endpoint {
                    print("[AudioReceiver] Bonjour found: \(name)")
                    done = true
                    browser.cancel()
                    // Resolve to IP via NetService
                    let svc = NetService(domain: "local.", type: "_soluna._tcp.", name: name)
                    let resolver = ServiceResolver(service: svc) { [weak self] host in
                        Task { @MainActor in
                            guard let self else { return }
                            if let host = host {
                                print("[AudioReceiver] LAN solunad IP: \(host)")
                                self.connectRelay(group: group, host: host, port: 5099)
                            } else {
                                print("[AudioReceiver] Resolve failed, WAN fallback")
                                self.connectRelay(group: group)
                            }
                        }
                    }
                    Task { @MainActor in self.lanResolver = resolver }
                    resolver.resolve()
                    return
                }
            }
        }

        browser.stateUpdateHandler = { state in
            print("[AudioReceiver] NWBrowser state: \(state)")
        }

        browser.start(queue: .main)

        // 2s timeout → WAN fallback
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { [weak self] in
            guard let self, !done else { return }
            done = true
            browser.cancel()
            print("[AudioReceiver] Bonjour timeout, WAN fallback")
            self.connectRelay(group: group)
        }
    }

    func disconnectRelay() {
        lanBrowser?.cancel()
        lanBrowser = nil
        receiver.disconnectRelay()
        updateRelayState()
    }

    private func updateRelayState() {
        relayState = RelayState(from: receiver.relayState)
        relayGroup = receiver.relayGroup
        relayError = receiver.relayError
    }

    /// Toggle play/stop
    func toggle() {
        if isPlaying {
            stop()
        } else {
            start()
        }
    }

    /// Auto-start on app launch (called from ContentView.onAppear)
    func autoStart() {
        guard state == .stopped else { return }
        start()
    }

    // MARK: - Network Stats Polling

    func startStatsPolling() {
        stopStatsPolling()
        statsTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                self.networkLatencyMs = self.receiver.networkLatencyMs
                self.jitterMs = self.receiver.jitterMs
                self.packetLossPercent = self.receiver.packetLossPercent
            }
        }
        // Level meter at ~30fps for smooth visualization
        levelTimer = Timer.scheduledTimer(withTimeInterval: 0.033, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                self.outputLevel = self.receiver.outputPeakLevel
            }
        }
    }

    func stopStatsPolling() {
        statsTimer?.invalidate()
        statsTimer = nil
        levelTimer?.invalidate()
        levelTimer = nil
    }

    // MARK: - Watchdog (auto-reconnect on stream loss)

    private func startWatchdog() {
        stopWatchdog()
        lastPacketCount = packetsReceived
        staleTicks = 0
        watchdogTimer = Timer.scheduledTimer(withTimeInterval: 3.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.watchdogTick()
            }
        }
    }

    private func stopWatchdog() {
        watchdogTimer?.invalidate()
        watchdogTimer = nil
        staleTicks = 0
    }

    private func watchdogTick() {
        guard state == .receiving || state == .connecting else { return }
        // Don't reconnect while mic is transmitting — reconnect kills the mic
        guard !isMicTransmitting else { return }
        // Don't reconnect in file-sync mode — no RTP packets expected
        guard currentSyncFile == nil else { return }

        if packetsReceived == lastPacketCount {
            staleTicks += 1
            // 10 ticks × 3s = 30 seconds with no new packets → reconnect
            if staleTicks >= 10 && state == .receiving {
                print("[AudioReceiver] Watchdog: no packets for \(staleTicks * 3)s — reconnecting")
                reconnect()
            }
        } else {
            staleTicks = 0
            lastPacketCount = packetsReceived
        }
    }

    /// Reconnect: stop then start with a brief delay
    private func reconnect() {
        stop()
        Task {
            try? await Task.sleep(nanoseconds: 1_000_000_000) // 1s
            start()
        }
    }

    // MARK: - Auto-Reconnect (network / interruption)

    private func setupNetworkMonitor() {
        networkMonitor.pathUpdateHandler = { [weak self] path in
            Task { @MainActor in
                guard let self else { return }
                if path.status == .satisfied && self.wasPlayingBeforeDisconnect {
                    print("[AudioReceiver] Network restored — reconnecting")
                    self.wasPlayingBeforeDisconnect = false
                    // Brief delay for WiFi to stabilize
                    try? await Task.sleep(nanoseconds: 1_500_000_000)
                    self.start()
                } else if path.status != .satisfied && self.isPlaying {
                    print("[AudioReceiver] Network lost — will reconnect when available")
                    self.wasPlayingBeforeDisconnect = true
                    self.stop()
                }
            }
        }
        networkMonitor.start(queue: DispatchQueue.global(qos: .utility))
    }

    private func setupAudioInterruptionHandler() {
        interruptionObserver = NotificationCenter.default.addObserver(
            forName: AVAudioSession.interruptionNotification,
            object: nil, queue: .main
        ) { [weak self] notification in
            guard let self,
                  let info = notification.userInfo,
                  let typeValue = info[AVAudioSessionInterruptionTypeKey] as? UInt,
                  let type = AVAudioSession.InterruptionType(rawValue: typeValue) else { return }

            Task { @MainActor in
                // Skip interruptions caused by our own session category switch (mic toggle)
                if self.suppressInterruption {
                    print("[AudioReceiver] Ignoring interruption (mic toggle in progress)")
                    return
                }
                if type == .ended {
                    print("[AudioReceiver] Audio interruption ended — resuming")
                    try? AVAudioSession.sharedInstance().setActive(true)
                    if self.wasPlayingBeforeDisconnect {
                        self.wasPlayingBeforeDisconnect = false
                        self.start()
                    }
                } else if type == .began {
                    // While mic is transmitting, ignore interruptions —
                    // the session change itself can trigger delayed interruptions.
                    // Real interruptions (phone call) will stop AudioUnits at the OS level.
                    if self.isMicTransmitting {
                        print("[AudioReceiver] Ignoring interruption (mic is transmitting)")
                        return
                    }
                    print("[AudioReceiver] Audio interruption began")
                    if self.isPlaying {
                        self.wasPlayingBeforeDisconnect = true
                        self.stop()
                    }
                }
            }
        }
    }

    private func setupRouteChangeHandler() {
        routeChangeObserver = NotificationCenter.default.addObserver(
            forName: AVAudioSession.routeChangeNotification,
            object: nil, queue: .main
        ) { [weak self] _ in
            self?.updateOutputLatency()
        }
    }

    private func updateOutputLatency() {
        let session = AVAudioSession.sharedInstance()
        let latencyMs = Float(session.outputLatency * 1000.0)
        receiver.outputLatencyMs = latencyMs
        let routeName = session.currentRoute.outputs.first?.portType.rawValue ?? "unknown"
        print("[AudioReceiver] Output route: \(routeName), latency: \(String(format: "%.1f", latencyMs)) ms")
    }

    // MARK: - Audio Fingerprinting

    /// Install a sample tap on the audio output and start fingerprint reporting.
    private func setupFingerprintTap(channel: String) {
        let fingerprinter = AudioFingerprint.shared
        fingerprinter.start(channel: channel)

        // Tap rendered audio — callback runs on the audio thread.
        // Down-mix to mono before feeding to the fingerprinter.
        receiver.setSampleTapCallback { samples, sampleCount, channels in
            if channels <= 1 {
                // Already mono
                fingerprinter.addSamples(samples, count: Int(sampleCount))
            } else {
                // Down-mix interleaved multi-channel to mono
                let ch = Int(channels)
                let frames = Int(sampleCount) / ch
                // Stack-allocate via withUnsafeTemporaryAllocation for audio-thread safety
                withUnsafeTemporaryAllocation(of: Float.self, capacity: frames) { mono in
                    for i in 0..<frames {
                        var sum: Float = 0
                        for c in 0..<ch {
                            sum += samples[i * ch + c]
                        }
                        mono[i] = sum / Float(ch)
                    }
                    fingerprinter.addSamples(mono.baseAddress!, count: frames)
                }
            }
        }
    }

    /// Remove the sample tap and stop fingerprint reporting.
    private func teardownFingerprintTap() {
        receiver.setSampleTapCallback(nil)
        AudioFingerprint.shared.stop()
    }

    // MARK: - Internal delegate handling

    fileprivate func handleStateChange(_ newState: SolunaReceiverState) {
        self.state = State(from: newState)
    }

    fileprivate func handleStatsUpdate(_ stats: SolunaReceiverStats) {
        self.packetsReceived  = stats.packetsReceived
        self.packetsDropped   = stats.packetsDropped
        self.packetsConcealed = stats.packetsConcealed
        self.deviceHealth     = receiver.deviceHealth
        self.txPacketsSent    = receiver.txPacketsSent
        self.micInputLevel    = receiver.micInputLevel
        // Read back adaptive buffer target from C++ (may have been auto-adjusted)
        let currentTarget = receiver.bufferTargetMs
        if currentTarget != bufferMs {
            bufferMs = currentTarget
        }
        // isMicTransmitting is managed by toggleMic()/stop() only.
        // Don't overwrite from bridge — it can cause false negatives
        // during session transitions.
        updateRelayState()
        // Poll debug log from C++ bridge
        debugLog = receiver.debugLog ?? ""
    }

    fileprivate func handleError(_ error: Error) {
        self.errorMessage = error.localizedDescription
    }
}

// MARK: - Delegate Handler

/// NSObject subclass to handle Objective-C delegate callbacks
private final class DelegateHandler: NSObject, SolunaReceiverDelegate {
    weak var audioReceiver: AudioReceiver?

    func receiverDidChange(_ state: SolunaReceiverState) {
        Task { @MainActor in
            audioReceiver?.handleStateChange(state)
        }
    }

    func receiverDidUpdate(_ stats: SolunaReceiverStats) {
        Task { @MainActor in
            audioReceiver?.handleStatsUpdate(stats)
        }
    }

    func receiverDidEncounter(_ error: Error) {
        Task { @MainActor in
            audioReceiver?.handleError(error)
        }
    }
}

// MARK: - Bonjour service resolver

private class ServiceResolver: NSObject, NetServiceDelegate {
    private let service: NetService
    private let completion: (String?) -> Void
    private var done = false

    init(service: NetService, completion: @escaping (String?) -> Void) {
        self.service = service
        self.completion = completion
        super.init()
    }

    func resolve() {
        service.delegate = self
        service.resolve(withTimeout: 2.0)
    }

    func netServiceDidResolveAddress(_ sender: NetService) {
        guard !done else { return }
        if let addresses = sender.addresses {
            for data in addresses {
                data.withUnsafeBytes { ptr in
                    let sa = ptr.baseAddress!.assumingMemoryBound(to: sockaddr.self)
                    if sa.pointee.sa_family == UInt8(AF_INET) {
                        let sin = ptr.baseAddress!.assumingMemoryBound(to: sockaddr_in.self)
                        var addr = sin.pointee.sin_addr
                        var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                        inet_ntop(AF_INET, &addr, &buf, socklen_t(INET_ADDRSTRLEN))
                        done = true
                        completion(String(cString: buf))
                    }
                }
            }
        }
    }

    func netService(_ sender: NetService, didNotResolve errorDict: [String: NSNumber]) {
        guard !done else { return }
        done = true
        completion(nil)
    }
}
