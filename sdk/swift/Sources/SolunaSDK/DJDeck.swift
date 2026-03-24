import Foundation
import AVFoundation

// MARK: - DJDeckState

/// State of a single DJ deck.
public enum DJDeckState: Sendable {
    case empty
    case loaded
    case playing
    case paused
}

// MARK: - Single Deck

/// A single audio file playback deck.
///
/// Uses AVAudioFile to read audio files and converts to 48kHz stereo float
/// for mixing. Supports play/pause/seek.
public final class Deck {

    /// Current state of the deck.
    public private(set) var state: DJDeckState = .empty

    /// Track name (filename without path).
    public private(set) var trackName: String = ""

    /// Playback progress from 0.0 to 1.0.
    public var progress: Float {
        guard totalFrames > 0 else { return 0 }
        return Float(playedFrames) / Float(totalFrames)
    }

    /// Whether the deck is currently playing (not paused).
    public var isPlaying: Bool { state == .playing }

    // MARK: - Private

    private var audioFile: AVAudioFile?
    private var totalFrames: AVAudioFramePosition = 0
    private var playedFrames: AVAudioFramePosition = 0

    /// Output format: 48kHz stereo float32 non-interleaved.
    private let outputFormat = AVAudioFormat(
        commonFormat: .pcmFormatFloat32,
        sampleRate: 48000,
        channels: 2,
        interleaved: false
    )!

    public init() {}

    // MARK: - Load / Close

    /// Load an audio file into the deck.
    ///
    /// - Parameter url: URL of the audio file (MP3, AAC, WAV, etc.).
    /// - Returns: True if the file was loaded successfully.
    @discardableResult
    public func load(url: URL) -> Bool {
        close()

        do {
            let file = try AVAudioFile(forReading: url)
            self.audioFile = file
            self.totalFrames = file.length
            self.playedFrames = 0
            self.trackName = url.lastPathComponent
            self.state = .loaded
            return true
        } catch {
            print("[SolunaSDK] DJDeck load error: \(error)")
            return false
        }
    }

    /// Close the current file and reset the deck.
    public func close() {
        audioFile = nil
        totalFrames = 0
        playedFrames = 0
        trackName = ""
        state = .empty
    }

    // MARK: - Playback Control

    /// Start or resume playback.
    public func play() {
        guard state == .loaded || state == .paused else { return }
        state = .playing
    }

    /// Pause playback.
    public func pause() {
        guard state == .playing else { return }
        state = .paused
    }

    /// Toggle play/pause.
    public func togglePlayPause() {
        if state == .playing { pause() }
        else if state == .loaded || state == .paused { play() }
    }

    /// Seek to a position (0.0 to 1.0).
    public func seek(to position: Float) {
        guard let file = audioFile, totalFrames > 0 else { return }
        let targetFrame = AVAudioFramePosition(Float(totalFrames) * max(0, min(1, position)))
        file.framePosition = targetFrame
        playedFrames = targetFrame
    }

    // MARK: - Read Frames

    /// Read audio frames from the file into a float buffer.
    ///
    /// - Parameters:
    ///   - buffer: Destination buffer (interleaved stereo float).
    ///   - frameCount: Number of frames to read.
    /// - Returns: Number of frames actually read (0 if paused, empty, or EOF).
    func readFrames(into buffer: AVAudioPCMBuffer, frameCount: AVAudioFrameCount) -> AVAudioFrameCount {
        guard let file = audioFile, state == .playing else { return 0 }

        do {
            buffer.frameLength = 0
            try file.read(into: buffer, frameCount: frameCount)
            let framesRead = buffer.frameLength
            playedFrames += AVAudioFramePosition(framesRead)

            // Check EOF
            if framesRead == 0 || playedFrames >= totalFrames {
                state = .loaded
                // Reset to beginning for potential replay
                file.framePosition = 0
                playedFrames = 0
            }

            return framesRead
        } catch {
            return 0
        }
    }
}

// MARK: - DJDeckController

/// Dual-deck DJ controller with crossfade support.
///
/// Reads audio from two decks, applies equal-power crossfade, converts to
/// int16 PCM, and sends as OSTP packets through the relay connection.
///
/// Based on the C++ DJController implementation with the same mix_loop
/// architecture: 960 frames (20ms) per packet at 48kHz stereo.
public final class DJDeckController {

    // MARK: - Public State

    /// Deck A instance.
    public let deckA = Deck()

    /// Deck B instance.
    public let deckB = Deck()

    /// Crossfader position (0.0 = full A, 0.5 = equal, 1.0 = full B).
    public var crossfader: Float = 0.5 {
        didSet { crossfader = max(0, min(1, crossfader)) }
    }

    /// Whether the mix loop is running.
    public private(set) var isActive = false

    // MARK: - Private

    private weak var connection: RelayConnection?
    private var ssrc: UInt32
    private var sequenceNumber: UInt16 = 0
    private var rtpTimestamp: UInt32 = 0
    private var mixThread: Thread?
    private let running = AtomicFlag()

    /// Output format for reading from decks: 48kHz stereo float32 non-interleaved.
    private let outputFormat = AVAudioFormat(
        commonFormat: .pcmFormatFloat32,
        sampleRate: 48000,
        channels: 2,
        interleaved: false
    )!

    /// Frames per mix packet (960 = 20ms at 48kHz, matching C++ DJController).
    private let framesPerPacket: Int = 960

    // MARK: - Init

    public init() {
        ssrc = UInt32.random(in: 0...UInt32.max)
    }

    // MARK: - Lifecycle

    /// Start the mix loop. Audio from both decks will be mixed and sent.
    ///
    /// - Parameter connection: The active RelayConnection to send packets through.
    public func start(connection: RelayConnection) {
        guard !isActive else { return }
        self.connection = connection
        running.set(true)
        isActive = true

        let thread = Thread { [weak self] in
            self?.mixLoop()
        }
        thread.qualityOfService = .userInteractive
        thread.start()
        mixThread = thread
    }

    /// Stop the mix loop and close both decks.
    public func stop() {
        running.set(false)
        isActive = false
        // Thread will exit naturally when running becomes false
        mixThread = nil
    }

    // MARK: - Deck Controls

    /// Load a file into Deck A and start playing.
    @discardableResult
    public func loadDeckA(url: URL) -> Bool {
        guard deckA.load(url: url) else { return false }
        deckA.play()
        return true
    }

    /// Load a file into Deck B and start playing.
    @discardableResult
    public func loadDeckB(url: URL) -> Bool {
        guard deckB.load(url: url) else { return false }
        deckB.play()
        return true
    }

    // MARK: - Mix Loop

    private func mixLoop() {
        let kFrames = framesPerPacket
        let kChannels = 2
        let interval: TimeInterval = Double(kFrames) / 48000.0

        guard let bufferA = AVAudioPCMBuffer(pcmFormat: outputFormat, frameCapacity: AVAudioFrameCount(kFrames)),
              let bufferB = AVAudioPCMBuffer(pcmFormat: outputFormat, frameCapacity: AVAudioFrameCount(kFrames)) else {
            running.set(false)
            isActive = false
            return
        }

        var nextTime = Date()

        while running.value {
            nextTime = nextTime.addingTimeInterval(interval)

            // Read from both decks
            let framesA = deckA.readFrames(into: bufferA, frameCount: AVAudioFrameCount(kFrames))
            let framesB = deckB.readFrames(into: bufferB, frameCount: AVAudioFrameCount(kFrames))

            // Equal-power crossfade: gain_a = cos(cf * pi/2), gain_b = sin(cf * pi/2)
            let cf = crossfader
            let gainA = cosf(cf * .pi / 2.0)
            let gainB = sinf(cf * .pi / 2.0)

            // Mix into int16 PCM (interleaved stereo)
            var mixOut = [Int16](repeating: 0, count: kFrames * kChannels)

            for ch in 0..<kChannels {
                let dataA = bufferA.floatChannelData?[ch]
                let dataB = bufferB.floatChannelData?[ch]

                for f in 0..<kFrames {
                    let sA: Float = (f < Int(framesA) && dataA != nil) ? dataA![f] : 0
                    let sB: Float = (f < Int(framesB) && dataB != nil) ? dataB![f] : 0
                    let mixed = sA * gainA + sB * gainB
                    let clamped = max(-1.0, min(1.0, mixed))
                    mixOut[f * kChannels + ch] = Int16(clamped * 32767.0)
                }
            }

            // Only send if at least one deck is playing
            if framesA > 0 || framesB > 0 {
                sendPacket(pcm: mixOut)
            }

            // Pace in real-time
            let now = Date()
            if nextTime > now {
                Thread.sleep(until: nextTime)
            }
        }

        DispatchQueue.main.async { [weak self] in
            self?.isActive = false
        }
    }

    private func sendPacket(pcm: [Int16]) {
        guard let connection else { return }

        // Convert int16 array to little-endian byte payload
        var payload = Data(capacity: pcm.count * 2)
        for sample in pcm {
            var le = sample.littleEndian
            withUnsafeBytes(of: &le) { payload.append(contentsOf: $0) }
        }

        let packet = OSTPacketBuilder.buildPacket(
            payload: payload,
            payloadType: 96,
            sequenceNumber: sequenceNumber,
            timestamp: rtpTimestamp,
            ssrc: ssrc,
            channels: 2,
            deckId: 0
        )

        connection.sendRawData(packet)

        sequenceNumber &+= 1
        rtpTimestamp += UInt32(framesPerPacket)
    }
}
