import Foundation
import AVFoundation

/// Manages AVAudioEngine playback using AVAudioSourceNode (pull-based).
///
/// Architecture: UDP recv thread → decode → ring buffer → AVAudioSourceNode callback
/// CoreAudio pulls from the ring buffer at its own pace. No scheduleBuffer, no gaps.
///
/// On iOS, configures AVAudioSession for .playback mode.
/// Supports mic monitoring (karaoke mode) by mixing mic input at low latency.
final class AudioPlayer {

    // MARK: - Audio Format

    private let playbackFormat: AVAudioFormat
    private let sampleRate: Double = 48000

    // MARK: - Engine

    private var audioEngine: AVAudioEngine?
    private var sourceNode: AVAudioSourceNode?

    /// Mic monitoring state
    private var micMonitoringActive = false

    /// Whether the engine is currently running.
    var isPlaying: Bool { audioEngine?.isRunning ?? false }

    // MARK: - Lock-Free Ring Buffer (SPSC)

    /// Ring buffer capacity in mono Float samples. 4 seconds @ 48kHz.
    private let ringCapacity = 192000
    private var ringBuffer: UnsafeMutablePointer<Float>
    /// Write position (only modified by producer / recv thread)
    private var writePos: Int64 = 0
    /// Read position (only modified by consumer / audio callback)
    private var readPos: Int64 = 0
    /// Pre-buffer threshold before playback starts (100ms = 4800 samples)
    private let prefillThreshold = 4800
    private var prefilled = false

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
        stop()
        ringBuffer.deallocate()
    }

    // MARK: - Ring Buffer Operations (lock-free)

    private func ringAvailable() -> Int {
        let w = OSAtomicAdd64(0, &writePos)
        let r = OSAtomicAdd64(0, &readPos)
        return Int(w - r)
    }

    /// Write mono float samples into ring buffer. Called from recv thread.
    func writeSamples(_ samples: UnsafePointer<Float>, count: Int) {
        let w = Int(OSAtomicAdd64(0, &writePos))
        let cap = ringCapacity
        // Drop if ring is nearly full (leave 10% headroom)
        if ringAvailable() > cap - cap / 10 { return }
        for i in 0..<count {
            ringBuffer[(w + i) % cap] = samples[i]
        }
        OSAtomicAdd64(Int64(count), &writePos)
    }

    /// Write decoded float samples from a packet. Convenience for [Float].
    func writeSamples(_ samples: [Float]) {
        samples.withUnsafeBufferPointer { buf in
            if let ptr = buf.baseAddress {
                writeSamples(ptr, count: samples.count)
            }
        }
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

    /// Reset ring buffer (call when switching channels)
    func flush() {
        writePos = 0
        readPos = 0
        prefilled = false
    }

    // MARK: - Start / Stop

    func start() {
        guard audioEngine == nil else { return }
        configureAudioSession()
        flush()

        let engine = AVAudioEngine()

        // AVAudioSourceNode: CoreAudio pulls from our ring buffer
        let node = AVAudioSourceNode(format: playbackFormat) { [weak self] _, _, frameCount, bufferList -> OSStatus in
            guard let self else { return noErr }
            let frames = Int(frameCount)
            let ablp = UnsafeMutableAudioBufferListPointer(bufferList)

            // Wait for prefill before starting playback
            let avail = self.ringAvailable()
            if !self.prefilled {
                if avail < self.prefillThreshold {
                    // Output silence during prefill
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

            // Copy to stereo channels (mono → both L and R)
            for ch in 0..<ablp.count {
                if let dst = ablp[ch].mData?.assumingMemoryBound(to: Float.self) {
                    if got > 0 {
                        memcpy(dst, tmp, got * MemoryLayout<Float>.size)
                    }
                    // Zero-fill remainder on underrun (smooth, no click)
                    if got < frames {
                        memset(dst.advanced(by: got), 0, (frames - got) * MemoryLayout<Float>.size)
                    }
                }
            }
            return noErr
        }

        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: playbackFormat)

        do {
            try engine.start()
        } catch {
            print("[SolunaSDK] AudioEngine start error: \(error)")
            return
        }

        self.audioEngine = engine
        self.sourceNode = node
    }

    func stop() {
        stopMicMonitoring()
        if let engine = audioEngine, engine.isRunning {
            engine.stop()
        }
        if let node = sourceNode, let engine = audioEngine {
            engine.detach(node)
        }
        sourceNode = nil
        audioEngine = nil
        flush()

        #if os(iOS)
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        #endif
    }

    // MARK: - Decode + Write Helpers

    /// Decode S24 (int32 LE) payload and write to ring buffer.
    /// S24 values are in the lower 24 bits of each int32.
    func writeS24Payload(_ payload: ArraySlice<UInt8>, channels: Int) {
        let bytesPerSample = 4
        let totalSamples = payload.count / bytesPerSample
        guard totalSamples > 0 else { return }
        let framesPerChannel = totalSamples / max(channels, 1)

        var samples = [Float]()
        samples.reserveCapacity(framesPerChannel)

        let scale: Float = 1.0 / 8388608.0  // 2^23 for S24
        let base = payload.startIndex

        for f in 0..<framesPerChannel {
            // Take first channel (mono) or left channel
            let byteOffset = base + f * channels * bytesPerSample
            guard byteOffset + 3 < payload.endIndex else { break }
            let val = Int32(payload[byteOffset]) |
                      (Int32(payload[byteOffset + 1]) << 8) |
                      (Int32(payload[byteOffset + 2]) << 16) |
                      (Int32(payload[byteOffset + 3]) << 24)
            // S24 in int32: shift right 8 to get 24-bit range, then scale
            samples.append(Float(val) * scale)
        }

        writeSamples(samples)
    }

    /// Decode ADPCM payload and write to ring buffer.
    func writeADPCMPayload(_ payload: ArraySlice<UInt8>, channels: Int) {
        guard let pcmBytes = ADPCMCodec.decodePayload(Array(payload)) else { return }
        let totalSamples = pcmBytes.count / 2
        guard totalSamples > 0 else { return }
        let framesPerChannel = totalSamples / max(channels, 1)

        var samples = [Float]()
        samples.reserveCapacity(framesPerChannel)

        for f in 0..<framesPerChannel {
            let byteIdx = f * max(channels, 1) * 2
            guard byteIdx + 1 < pcmBytes.count else { break }
            let s16 = Int16(bitPattern: UInt16(pcmBytes[byteIdx]) | (UInt16(pcmBytes[byteIdx + 1]) << 8))
            samples.append(Float(s16) / 32768.0)
        }

        writeSamples(samples)
    }

    /// Decode Opus payload and write to ring buffer.
    func writeOpusPayload(_ payload: ArraySlice<UInt8>, channels: Int) {
        let payloadArray = Array(payload)
        guard let pcmBuffer = bufferFromOpusPayload(payloadArray, channels: max(channels, 1)) else { return }
        guard let data = pcmBuffer.floatChannelData?[0] else { return }
        writeSamples(data, count: Int(pcmBuffer.frameLength))
    }

    // MARK: - Legacy scheduleBuffer (kept for compatibility)

    /// Schedule a PCM buffer for playback (legacy, prefer writeSamples).
    func scheduleBuffer(_ buffer: AVAudioPCMBuffer) {
        // Convert to ring buffer write
        guard let data = buffer.floatChannelData?[0] else { return }
        writeSamples(data, count: Int(buffer.frameLength))
    }

    /// Create a stereo float32 PCM buffer from int32 LE payload (legacy).
    func bufferFromInt32Payload(_ payload: [UInt8], channels: Int) -> AVAudioPCMBuffer? {
        let totalSamples = payload.count / 4
        guard totalSamples > 0 else { return nil }
        let framesPerChannel = totalSamples / max(channels, 1)
        guard framesPerChannel > 0 else { return nil }
        guard let buffer = AVAudioPCMBuffer(pcmFormat: playbackFormat, frameCapacity: AVAudioFrameCount(framesPerChannel)) else { return nil }
        buffer.frameLength = AVAudioFrameCount(framesPerChannel)
        let scale: Float = 1.0 / Float(Int32.max)
        for ch in 0..<min(channels, 2) {
            guard let channelData = buffer.floatChannelData?[ch] else { continue }
            for f in 0..<framesPerChannel {
                let sampleIdx = f * channels + ch
                let byteOffset = sampleIdx * 4
                guard byteOffset + 3 < payload.count else { break }
                let val = Int32(payload[byteOffset]) | (Int32(payload[byteOffset + 1]) << 8) |
                          (Int32(payload[byteOffset + 2]) << 16) | (Int32(payload[byteOffset + 3]) << 24)
                channelData[f] = Float(val) * scale
            }
        }
        if channels == 1, let left = buffer.floatChannelData?[0], let right = buffer.floatChannelData?[1] {
            memcpy(right, left, Int(framesPerChannel) * MemoryLayout<Float>.size)
        }
        return buffer
    }

    /// Create a stereo float32 PCM buffer from ADPCM payload (legacy).
    func bufferFromADPCMPayload(_ payload: [UInt8], channels: Int) -> AVAudioPCMBuffer? {
        guard let pcmBytes = ADPCMCodec.decodePayload(payload) else { return nil }
        let totalSamples = pcmBytes.count / 2
        guard totalSamples > 0 else { return nil }
        let framesPerChannel = totalSamples / max(channels, 1)
        guard framesPerChannel > 0 else { return nil }
        guard let buffer = AVAudioPCMBuffer(pcmFormat: playbackFormat, frameCapacity: AVAudioFrameCount(framesPerChannel)) else { return nil }
        buffer.frameLength = AVAudioFrameCount(framesPerChannel)
        for ch in 0..<min(channels, 2) {
            guard let channelData = buffer.floatChannelData?[ch] else { continue }
            for f in 0..<framesPerChannel {
                let byteIdx = (f * channels + ch) * 2
                guard byteIdx + 1 < pcmBytes.count else { break }
                let s16 = Int16(bitPattern: UInt16(pcmBytes[byteIdx]) | (UInt16(pcmBytes[byteIdx + 1]) << 8))
                channelData[f] = Float(s16) / 32768.0
            }
        }
        if channels == 1, let left = buffer.floatChannelData?[0], let right = buffer.floatChannelData?[1] {
            memcpy(right, left, Int(framesPerChannel) * MemoryLayout<Float>.size)
        }
        return buffer
    }

    /// Create a stereo float32 PCM buffer from Opus payload (legacy).
    func bufferFromOpusPayload(_ payload: [UInt8], channels: Int) -> AVAudioPCMBuffer? {
        var opusDesc = AudioStreamBasicDescription(
            mSampleRate: 48000, mFormatID: kAudioFormatOpus, mFormatFlags: 0,
            mBytesPerPacket: 0, mFramesPerPacket: 960, mBytesPerFrame: 0,
            mChannelsPerFrame: UInt32(channels), mBitsPerChannel: 0, mReserved: 0
        )
        guard let opusFormat = AVAudioFormat(streamDescription: &opusDesc),
              let converter = AVAudioConverter(from: opusFormat, to: playbackFormat) else { return nil }
        let compressedBuffer = AVAudioCompressedBuffer(format: opusFormat, packetCapacity: 1, maximumPacketSize: payload.count)
        compressedBuffer.byteLength = UInt32(payload.count)
        compressedBuffer.packetCount = 1
        memcpy(compressedBuffer.data, payload, payload.count)
        compressedBuffer.packetDescriptions![0].mStartOffset = 0
        compressedBuffer.packetDescriptions![0].mDataByteSize = UInt32(payload.count)
        compressedBuffer.packetDescriptions![0].mVariableFramesInPacket = 960
        let pcmBuffer = AVAudioPCMBuffer(pcmFormat: playbackFormat, frameCapacity: 960)!
        var error: NSError?
        converter.convert(to: pcmBuffer, error: &error) { _, outStatus in
            outStatus.pointee = .haveData; return compressedBuffer
        }
        if error != nil { return nil }
        return pcmBuffer
    }

    func bufferFromLC3Payload(_ payload: [UInt8], channels: Int) -> AVAudioPCMBuffer? { nil }

    // MARK: - Mic Monitoring (Karaoke Mode)

    func startMicMonitoring() {
        guard !micMonitoringActive, let engine = audioEngine else { return }
        #if os(iOS)
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker, .allowBluetooth])
            try session.setActive(true)
        } catch {
            print("[SolunaSDK] Mic session error: \(error)")
            return
        }
        #endif
        let inputNode = engine.inputNode
        let inputFormat = inputNode.outputFormat(forBus: 0)
        engine.connect(inputNode, to: engine.mainMixerNode, format: inputFormat)
        if !engine.isRunning { try? engine.start() }
        micMonitoringActive = true
    }

    func stopMicMonitoring() {
        guard micMonitoringActive, let engine = audioEngine else { return }
        micMonitoringActive = false
        let inputNode = engine.inputNode
        engine.disconnectNodeInput(inputNode)
        engine.disconnectNodeOutput(inputNode)
        #if os(iOS)
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default,
                                    options: [.defaultToSpeaker, .mixWithOthers, .allowBluetooth])
            try session.setActive(true)
        } catch {
            print("[SolunaSDK] Session revert error: \(error)")
        }
        #endif
    }

    // MARK: - Audio Session

    private func configureAudioSession() {
        #if os(iOS)
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default,
                                    options: [.defaultToSpeaker, .mixWithOthers, .allowBluetooth])
            try session.setPreferredSampleRate(48000)
            try session.setActive(true)
        } catch {
            print("[SolunaSDK] AudioSession error: \(error)")
        }
        #endif
    }
}
