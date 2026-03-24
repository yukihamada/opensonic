import Foundation
import AVFoundation

/// Captures microphone audio and sends it as OSTP packets via a relay connection.
///
/// Uses AVAudioEngine's inputNode to capture mic audio, converts to S24 format
/// (matching the C++ TransmitterImpl), and sends OSTP packets through the
/// existing RelayConnection's UDP socket.
///
/// Works on both iOS and macOS.
public final class MicTransmitter {

    // MARK: - State

    /// Whether the transmitter is currently capturing and sending.
    public private(set) var isTransmitting = false

    /// Current mic input peak level (0.0 to 1.0) for UI meters.
    public private(set) var peakLevel: Float = 0.0

    /// Number of packets sent since last start.
    public private(set) var packetsSent: UInt64 = 0

    // MARK: - Configuration

    /// Number of audio channels to transmit (1=mono, 2=stereo).
    public var channels: Int = 1

    /// Sample rate (always 48kHz to match OSTP).
    private let sampleRate: Double = 48000

    /// Frames per OSTP packet (240 = 5ms at 48kHz, matching LAN tier).
    private let framesPerPacket: Int = 240

    // MARK: - Private

    private var audioEngine: AVAudioEngine?
    private weak var connection: RelayConnection?
    private var ssrc: UInt32
    private var sequenceNumber: UInt16 = 0
    private var rtpTimestamp: UInt32 = 0

    /// Accumulation buffer for mic samples before sending a full packet.
    private var sampleBuffer: [Int32] = []
    private let bufferLock = NSLock()

    // MARK: - Init

    public init() {
        ssrc = UInt32.random(in: 0...UInt32.max)
    }

    // MARK: - Start / Stop

    /// Start capturing mic audio and transmitting via the given relay connection.
    ///
    /// - Parameter connection: The active RelayConnection to send packets through.
    /// - Returns: True if capture started successfully.
    @discardableResult
    public func start(connection: RelayConnection) -> Bool {
        guard !isTransmitting else { return true }
        self.connection = connection

        let engine = AVAudioEngine()
        let inputNode = engine.inputNode

        // Request mono float at 48kHz
        let inputFormat = inputNode.outputFormat(forBus: 0)

        // Install a tap on the input node to capture mic audio
        let tapFormat = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: inputFormat.sampleRate,
            channels: 1,
            interleaved: false
        )!

        inputNode.installTap(onBus: 0, bufferSize: AVAudioFrameCount(framesPerPacket), format: tapFormat) { [weak self] buffer, _ in
            self?.handleMicBuffer(buffer)
        }

        do {
            try engine.start()
        } catch {
            print("[SolunaSDK] MicTransmitter engine start error: \(error)")
            return false
        }

        self.audioEngine = engine
        isTransmitting = true
        packetsSent = 0
        sequenceNumber = 0
        rtpTimestamp = 0
        sampleBuffer.removeAll()

        return true
    }

    /// Stop capturing and transmitting.
    public func stop() {
        guard isTransmitting else { return }

        audioEngine?.inputNode.removeTap(onBus: 0)
        audioEngine?.stop()
        audioEngine = nil

        isTransmitting = false
        connection = nil
        peakLevel = 0
    }

    // MARK: - Audio Processing

    private func handleMicBuffer(_ buffer: AVAudioPCMBuffer) {
        guard let floatData = buffer.floatChannelData?[0] else { return }
        let frameCount = Int(buffer.frameLength)

        // Track peak level for UI
        var peak: Float = 0
        for i in 0..<frameCount {
            let abs = Swift.abs(floatData[i])
            if abs > peak { peak = abs }
        }
        // Exponential decay: fast attack, slow release
        if peak > peakLevel {
            peakLevel = peak
        } else {
            peakLevel = peakLevel * 0.85
        }

        // Convert mono float [-1.0, 1.0] to S24 int32 (24-bit range)
        // and duplicate to stereo if channels == 2
        bufferLock.lock()
        for i in 0..<frameCount {
            let sample = Int32(floatData[i] * 8388607.0)
            // Always at least 1 channel
            sampleBuffer.append(sample)
            if channels == 2 {
                sampleBuffer.append(sample) // duplicate mono to stereo
            }
        }

        // Send complete packets
        let samplesPerPacket = framesPerPacket * max(channels, 1)
        while sampleBuffer.count >= samplesPerPacket {
            let packetSamples = Array(sampleBuffer.prefix(samplesPerPacket))
            sampleBuffer.removeFirst(samplesPerPacket)
            bufferLock.unlock()

            sendPacket(samples: packetSamples)

            bufferLock.lock()
        }
        bufferLock.unlock()
    }

    private func sendPacket(samples: [Int32]) {
        guard let connection else { return }

        // Convert int32 array to little-endian byte payload
        var payload = Data(capacity: samples.count * 4)
        for sample in samples {
            var le = sample.littleEndian
            withUnsafeBytes(of: &le) { payload.append(contentsOf: $0) }
        }

        let packet = OSTPacketBuilder.buildPacket(
            payload: payload,
            payloadType: 96,
            sequenceNumber: sequenceNumber,
            timestamp: rtpTimestamp,
            ssrc: ssrc,
            channels: channels,
            deckId: 0
        )

        connection.sendRawData(packet)

        sequenceNumber &+= 1
        rtpTimestamp += UInt32(framesPerPacket)
        packetsSent += 1
    }
}
