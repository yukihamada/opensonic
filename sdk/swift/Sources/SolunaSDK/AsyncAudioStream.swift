import Foundation

/// A single frame of decoded PCM audio data.
public struct AudioSample: Sendable {
    /// Interleaved float32 PCM samples.
    public let samples: [Float]
    /// Number of audio channels.
    public let channels: Int
    /// Sample rate in Hz (typically 48000).
    public let sampleRate: Double
    /// RTP timestamp from the OSTP packet.
    public let timestamp: UInt32

    public init(samples: [Float], channels: Int, sampleRate: Double, timestamp: UInt32) {
        self.samples = samples
        self.channels = channels
        self.sampleRate = sampleRate
        self.timestamp = timestamp
    }
}

/// Events emitted by the Soluna client for async consumption.
public enum SolunaEvent: Sendable {
    /// Successfully connected to the relay.
    case connected
    /// Disconnected from the relay.
    case disconnected
    /// A new track started (from META: control message).
    case trackChanged(TrackInfo)
    /// Current audio level (RMS, 0.0 - 1.0).
    case audioLevel(Float)
}

/// Provides `AsyncStream`-based APIs for consuming audio data and events
/// using modern Swift concurrency.
///
/// Usage:
/// ```swift
/// let client = SolunaClient()
/// let streams = AsyncAudioStreams(client: client)
///
/// // Consume audio
/// Task {
///     for await sample in streams.audioStream {
///         process(sample.samples)
///     }
/// }
///
/// // Consume events
/// Task {
///     for await event in streams.eventStream {
///         switch event {
///         case .connected: print("Connected")
///         case .trackChanged(let track): print(track.title)
///         default: break
///         }
///     }
/// }
/// ```
public final class AsyncAudioStreams: SolunaClientDelegate {

    // MARK: - Private

    private var audioContinuation: AsyncStream<AudioSample>.Continuation?
    private var eventContinuation: AsyncStream<SolunaEvent>.Continuation?

    private let _audioStream: AsyncStream<AudioSample>
    private let _eventStream: AsyncStream<SolunaEvent>

    // MARK: - Public Streams

    /// An async stream of decoded PCM audio samples.
    public var audioStream: AsyncStream<AudioSample> { _audioStream }

    /// An async stream of connection and playback events.
    public var eventStream: AsyncStream<SolunaEvent> { _eventStream }

    // MARK: - Init

    /// Create async streams backed by a `SolunaClient`.
    ///
    /// Sets itself as the client's delegate. Only one delegate can be active,
    /// so this replaces any previously set delegate.
    ///
    /// - Parameter client: The `SolunaClient` to observe.
    @MainActor
    public init(client: SolunaClient) {
        var audioCont: AsyncStream<AudioSample>.Continuation!
        _audioStream = AsyncStream { continuation in
            audioCont = continuation
        }
        self.audioContinuation = audioCont

        var eventCont: AsyncStream<SolunaEvent>.Continuation!
        _eventStream = AsyncStream { continuation in
            eventCont = continuation
        }
        self.eventContinuation = eventCont

        client.delegate = self
    }

    deinit {
        audioContinuation?.finish()
        eventContinuation?.finish()
    }

    // MARK: - SolunaClientDelegate

    public func solunaClient(_ client: SolunaClient, didReceiveAudio samples: [Float], channels: Int, sampleRate: Double) {
        let sample = AudioSample(
            samples: samples,
            channels: channels,
            sampleRate: sampleRate,
            timestamp: 0
        )
        audioContinuation?.yield(sample)

        // Also emit audio level event
        if !samples.isEmpty {
            var rms: Float = 0
            for s in samples { rms += s * s }
            rms = sqrtf(rms / Float(samples.count))
            eventContinuation?.yield(.audioLevel(min(rms * 3.0, 1.0)))
        }
    }

    public func solunaClient(_ client: SolunaClient, didChangeState state: SolunaConnectionState) {
        switch state {
        case .connected:
            eventContinuation?.yield(.connected)
        case .disconnected:
            eventContinuation?.yield(.disconnected)
        default:
            break
        }
    }

    // MARK: - Manual Event Injection

    /// Manually inject a track-changed event (e.g. from WebhookManager).
    ///
    /// - Parameter track: The new track info.
    public func emitTrackChanged(_ track: TrackInfo) {
        eventContinuation?.yield(.trackChanged(track))
    }
}
