//
//  AudioReceiverBridge.h
//  SolunaReceiver
//
//  Objective-C bridge for Swift interop with C++ audio receiver
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Device health state based on underrun rate
typedef NS_ENUM(NSInteger, SolunaDeviceHealth) {
    SolunaDeviceHealthGood     = 0,  ///< Functioning normally
    SolunaDeviceHealthStressed = 1,  ///< High underrun rate, buffer auto-increased
    SolunaDeviceHealthSilenced = 2,  ///< Silenced to prevent noise (extreme underruns)
};

/// Receiver state enumeration
typedef NS_ENUM(NSInteger, SolunaReceiverState) {
    SolunaReceiverStateStopped = 0,
    SolunaReceiverStateConnecting,
    SolunaReceiverStateReceiving,
    SolunaReceiverStateError
};

/// Statistics from the receiver
@interface SolunaReceiverStats : NSObject
@property (nonatomic, readonly) uint64_t packetsReceived;
@property (nonatomic, readonly) uint64_t packetsDropped;
@property (nonatomic, readonly) uint64_t packetsConcealed;
@property (nonatomic, readonly) uint64_t sequenceErrors;
@property (nonatomic, readonly) uint64_t aes67Packets;
@property (nonatomic, readonly) uint64_t ostpPackets;
@end

/// Delegate protocol for receiver state changes
@protocol SolunaReceiverDelegate <NSObject>
@optional
- (void)receiverDidChange:(SolunaReceiverState)state;
- (void)receiverDidUpdate:(SolunaReceiverStats *)stats;
- (void)receiverDidEncounter:(NSError *)error;
@end

/// WAN relay connection state
typedef NS_ENUM(NSInteger, SolunaRelayState) {
    SolunaRelayStateDisconnected = 0,
    SolunaRelayStateConnecting,
    SolunaRelayStateConnected,
    SolunaRelayStateError
};

/// Main audio receiver bridge class
@interface SolunaAudioReceiver : NSObject

/// Current receiver state
@property (nonatomic, readonly) SolunaReceiverState state;

/// Volume (0.0 - 1.0)
@property (nonatomic, assign) float volume;

/// Muted state
@property (nonatomic, assign) BOOL muted;

/// Delegate for callbacks
@property (nonatomic, weak, nullable) id<SolunaReceiverDelegate> delegate;

/// Multicast group address (default: 239.69.0.1)
@property (nonatomic, copy) NSString *multicastGroup;

/// RTP port (default: 5004)
@property (nonatomic, assign) uint16_t port;

/// Number of channels (default: 1)
@property (nonatomic, assign) uint32_t channels;

/// Jitter buffer target in milliseconds (5–2000 ms, default 40 ms)
@property (nonatomic, assign) uint32_t bufferTargetMs;

/// Current device health (good / stressed / silenced)
@property (nonatomic, readonly) SolunaDeviceHealth deviceHealth;

/// When YES, the receive loop ignores multicast packets.
/// Audio only arrives via -injectRawPacket: (peer relay mode).
@property (nonatomic, assign) BOOL networkDisabled;

/// Singleton instance
+ (instancetype)sharedInstance;

/// Initialize with custom settings
- (instancetype)initWithMulticastGroup:(NSString *)group
                                  port:(uint16_t)port
                              channels:(uint32_t)channels;

/// Start receiving and playing audio
- (BOOL)start;

/// Stop receiving and playing audio
- (void)stop;

/// Get current statistics
- (SolunaReceiverStats *)currentStats;

// ── P2P Relay ──────────────────────────────────────────────────────────────

/// Set callback invoked for every raw RTP/OSTP packet received from the network.
/// Used in relay mode to forward packets to nearby peers via MultipeerConnectivity.
/// Pass nil to disable. Thread-safe.
- (void)setRelayCallback:(nullable void(^)(NSData * _Nonnull rawPacket))callback;

/// Inject a raw RTP/OSTP packet received from a relay peer.
/// The packet is parsed and fed directly into the audio ring buffer,
/// bypassing the UDP socket. Thread-safe.
- (void)injectRawPacket:(NSData * _Nonnull)data;

// ── WAN Relay ────────────────────────────────────────────────────────────

/// Connect to WAN relay server. Returns NO on immediate failure.
- (BOOL)connectToRelay:(NSString *)host port:(uint16_t)port
                 group:(NSString *)group password:(NSString *)password;

/// Disconnect from WAN relay.
- (void)disconnectRelay;

/// Current relay connection state.
@property (nonatomic, readonly) SolunaRelayState relayState;

/// Currently joined relay group name (nil if disconnected).
@property (nonatomic, readonly, copy, nullable) NSString *relayGroup;

/// Relay error message (nil if no error).
@property (nonatomic, readonly, copy, nullable) NSString *relayError;

// ── Mic Transmit (TX) ────────────────────────────────────────────────────

/// Whether mic transmit is currently active
@property (nonatomic, readonly) BOOL isMicTransmitting;

/// Number of TX packets sent
@property (nonatomic, readonly) uint64_t txPacketsSent;

/// Mic input peak level (0.0 - 1.0), updated per audio callback
@property (nonatomic, readonly) float micInputLevel;

/// Start capturing from the microphone and transmitting via OSTP multicast.
/// Switches AVAudioSession to .playAndRecord. Returns NO on failure.
- (BOOL)startMicTransmit;

/// Stop mic capture and transmission. Restores AVAudioSession to .playback.
- (void)stopMicTransmit;

@end

NS_ASSUME_NONNULL_END
