import Foundation

// MARK: - Connection State

/// The current state of the relay connection.
public enum SolunaConnectionState: Sendable {
    case disconnected
    case connecting
    case connected
    case error(String)
}

// MARK: - Channel Definition

/// A Soluna radio channel with metadata for UI display.
public struct SolunaChannel: Identifiable, Hashable, Sendable {
    public let id: String
    public let name: String
    public let emoji: String
    public let color: String  // hex color for UI tinting

    public init(id: String, name: String, emoji: String, color: String) {
        self.id = id
        self.name = name
        self.emoji = emoji
        self.color = color
    }
}

/// The 7 core Soluna radio channels broadcast from Hetzner VPS.
public enum SolunaChannels {
    public static let all: [SolunaChannel] = [
        SolunaChannel(id: "bjj",    name: "BJJ",     emoji: "🥋", color: "#E53E3E"),
        SolunaChannel(id: "soluna", name: "Soluna",   emoji: "🌀", color: "#ED8936"),
        SolunaChannel(id: "jazz",   name: "Jazz",     emoji: "🎷", color: "#D69E2E"),
        SolunaChannel(id: "chill",  name: "Chill",    emoji: "🌅", color: "#38B2AC"),
        SolunaChannel(id: "lofi",   name: "Lo-Fi",    emoji: "📻", color: "#805AD5"),
        SolunaChannel(id: "dance",  name: "Dance",    emoji: "💃", color: "#D53F8C"),
        SolunaChannel(id: "yuki",   name: "Yuki",     emoji: "❄️", color: "#63B3ED"),
    ]

    /// Look up a channel by id. Returns nil if not found.
    public static func channel(for id: String) -> SolunaChannel? {
        all.first { $0.id == id }
    }
}

// MARK: - Fan Rank

/// Fan rank based on cumulative listening minutes per channel.
public enum SolunaFanRank: String, Sendable, CaseIterable {
    case newFan     = "New Fan"
    case regular    = "Regular"
    case superfan   = "Superfan"
    case legend     = "Legend"

    /// Determine rank from total listening minutes.
    public static func from(minutes: Double) -> SolunaFanRank {
        switch minutes {
        case ..<30:    return .newFan
        case ..<120:   return .regular
        case ..<600:   return .superfan
        default:       return .legend
        }
    }

    /// Emoji badge for the rank.
    public var badge: String {
        switch self {
        case .newFan:   return "🌱"
        case .regular:  return "⭐"
        case .superfan: return "🔥"
        case .legend:   return "👑"
        }
    }
}

// MARK: - OSTP Constants

/// OSTP protocol constants matching the C++ implementation.
public enum OSTConstants {
    /// RTP header extension profile for OSTP ("OS" = 0x4F53).
    public static let ostpProfile: UInt16 = 0x4F53

    /// Default relay host.
    public static let defaultHost = "relay.solun.art"

    /// Default relay port.
    public static let defaultPort: UInt16 = 5100

    /// Default WebSocket URL for relay connection.
    public static let defaultWebSocketURL = "wss://relay.solun.art/ws/audio"

    /// Heartbeat interval in seconds (must be < relay's kStaleTimeoutSec=60).
    public static let heartbeatInterval: TimeInterval = 5.0

    /// Receive buffer size.
    public static let recvBufferSize = 16384

    /// RTP header size in bytes.
    public static let rtpHeaderSize = 12

    /// CRC-32 trailer size in bytes.
    public static let crcTrailerSize = 4

    /// S24 linear PCM payload type (24-bit in 32-bit int container).
    public static let ptS24: UInt8 = 96

    /// Float32 PCM payload type.
    public static let ptFloat32: UInt8 = 97

    /// Opus compressed payload type (RFC 6716, legacy).
    public static let ptOpus: UInt8 = 98

    /// Opus stereo payload type (OSTP spec).
    public static let ptOpusStereo: UInt8 = 111

    /// Opus mono payload type (OSTP spec).
    public static let ptOpusMono: UInt8 = 112

    /// ADPCM stereo payload type.
    public static let ptADPCMStereo: UInt8 = 115

    /// ADPCM mono payload type.
    public static let ptADPCMMono: UInt8 = 116

    /// LC3 Bluetooth LE Audio payload type (liblc3, Apache 2.0).
    public static let ptLC3: UInt8 = 119
}

// MARK: - Parsed OSTP Packet

/// A parsed OSTP/RTP audio packet.
public struct OSTPacket: Sendable {
    /// RTP payload type.
    public let payloadType: UInt8

    /// Number of audio channels (extracted from stream_id bits [13:10]).
    public let channels: Int

    /// Deck ID (extracted from stream_id bits [15:14]).
    public let deckId: Int

    /// Audio payload data (after header, before CRC trailer).
    public let payload: Data

    /// RTP sequence number.
    public let sequenceNumber: UInt16

    /// RTP timestamp.
    public let timestamp: UInt32
}

// MARK: - ADPCM State

/// IMA-ADPCM decoder state.
public struct ADPCMState {
    public var predicted: Int16 = 0
    public var stepIndex: UInt8 = 0

    public init() {}
    public init(predicted: Int16, stepIndex: UInt8) {
        self.predicted = predicted
        self.stepIndex = min(stepIndex, 88)
    }
}

// MARK: - Delegate Protocol

/// Delegate for receiving raw audio data instead of (or in addition to) built-in playback.
public protocol SolunaClientDelegate: AnyObject {
    /// Called when an audio packet is received and decoded to float32 PCM.
    /// - Parameters:
    ///   - client: The SolunaClient instance.
    ///   - samples: Interleaved float32 PCM samples.
    ///   - channels: Number of audio channels.
    ///   - sampleRate: Sample rate (always 48000).
    func solunaClient(_ client: SolunaClient, didReceiveAudio samples: [Float], channels: Int, sampleRate: Double)

    /// Called when connection state changes.
    func solunaClient(_ client: SolunaClient, didChangeState state: SolunaConnectionState)
}

// Default empty implementations
public extension SolunaClientDelegate {
    func solunaClient(_ client: SolunaClient, didReceiveAudio samples: [Float], channels: Int, sampleRate: Double) {}
    func solunaClient(_ client: SolunaClient, didChangeState state: SolunaConnectionState) {}
}
