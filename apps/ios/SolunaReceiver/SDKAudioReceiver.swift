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

    // MARK: - Ring Buffer (lock-free SPSC, 4s @ 48 kHz mono)

    private let ringCapacity = 192_000
    private let ringBuffer: UnsafeMutablePointer<Float>
    private var writePos: Int64 = 0
    private var readPos: Int64 = 0
    private let prefillThreshold = 4800  // 100 ms

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

            // Prefill gate
            let avail = self.ringAvailable()
            if avail < self.prefillThreshold && !self.firstPacketReceived.value {
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

            for ch in 0..<ablp.count {
                if let dst = ablp[ch].mData?.assumingMemoryBound(to: Float.self) {
                    if got > 0 { memcpy(dst, scratch, got * MemoryLayout<Float>.size) }
                    if got < frames { memset(dst.advanced(by: got), 0, (frames - got) * MemoryLayout<Float>.size) }
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
        if let engine = audioEngine, engine.isRunning { engine.stop() }
        if let node = sourceNode, let engine = audioEngine { engine.detach(node) }
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

            // 5ms recv timeout
            var tv = timeval(tv_sec: 0, tv_usec: 5000)
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
                self.sendUDP("JOIN:\(ch)::\(deviceName)\n")
            }
            timer.resume()
            self.heartbeatTimer = timer

            // Receive loop
            self.recvLoop()
        }
    }

    private func sendUDP(_ msg: String) {
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
            guard n > 12 else { continue }
            guard (buf[0] & 0xC0) == 0x80 else { continue }  // RTP version=2
            guard (buf[1] & 0x7F) == 96 else { continue }    // PT=96 (S24)

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
                DispatchQueue.main.async { [weak self] in
                    self?.packetsReceived = UInt64(pktCount)
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
