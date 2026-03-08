//
//  AudioReceiverBridge.h
//  SolunaReceiver
//
//  Objective-C bridge for Swift interop with C++ audio receiver
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Transport type of an audio output device
typedef NS_ENUM(NSInteger, SolunaTransportType) {
    SolunaTransportTypeBuiltIn   = 0,
    SolunaTransportTypeUsb       = 1,
    SolunaTransportTypeBluetooth = 2,
    SolunaTransportTypeAirPlay   = 3,
    SolunaTransportTypeVirtual   = 4,
    SolunaTransportTypeUnknown   = 255,
};

/// Information about a local audio output device
@interface SolunaDeviceInfo : NSObject
@property (nonatomic, readonly) uint32_t deviceId;
@property (nonatomic, readonly, copy) NSString *name;
@property (nonatomic, readonly) SolunaTransportType transportType;
@property (nonatomic, readonly) uint32_t outputChannels;
@property (nonatomic, readonly) uint32_t hardwareLatencyFrames;
@property (nonatomic, readonly) uint32_t safetyOffsetFrames;
/// Approximate hardware latency in milliseconds (at 48kHz)
@property (nonatomic, readonly) float hardwareLatencyMs;
/// Native sample rate of the device (Hz)
@property (nonatomic, readonly) double nativeSampleRate;
@end

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

// ── Local output devices ─────────────────────────────────────────────────

/// List all available local output devices (BT, AirPlay, USB, built-in)
+ (NSArray<SolunaDeviceInfo *> *)availableOutputDevices;

/// Add an extra output device by CoreAudio device ID. Returns sink index or -1.
- (int)addOutputDevice:(uint32_t)deviceId;

/// Remove an extra output device by CoreAudio device ID.
- (void)removeOutputDevice:(uint32_t)deviceId;

/// Set volume for an extra output (by sink index)
- (void)setVolume:(float)volume forOutput:(int)sinkIndex;

/// Set muted for an extra output (by sink index)
- (void)setMuted:(BOOL)muted forOutput:(int)sinkIndex;

/// Set delay in frames for an extra output (by sink index)
- (void)setDelay:(uint32_t)frames forOutput:(int)sinkIndex;

/// Set delay in frames for the primary output (This Mac)
- (void)setPrimaryDelay:(uint32_t)frames;

/// Number of active extra outputs
- (int)outputCount;

/// Get measured latency (ms) for an output device (EMA-smoothed, includes HW transport)
- (float)measuredLatencyForDevice:(uint32_t)deviceId;

/// VU meter: primary output RMS level (0.0-1.0)
- (float)primaryLevelRms;
/// VU meter: primary output peak level (0.0-1.0, with decay)
- (float)primaryLevelPeak;

/// Spectrum analyzer: 32-band FFT magnitudes (0.0-1.0). Returns NSArray of 32 NSNumber (float).
- (NSArray<NSNumber *> *)spectrumBands;
/// VU meter: extra output RMS level by device ID
- (float)levelRmsForDevice:(uint32_t)deviceId;
/// VU meter: extra output peak level by device ID
- (float)levelPeakForDevice:(uint32_t)deviceId;

/// Set L/R balance for an extra output (-1.0 = left, 0.0 = center, 1.0 = right)
- (void)setBalance:(float)balance forOutput:(int)sinkIndex;

/// Set L/R balance for the primary output (-1.0 = left, 0.0 = center, 1.0 = right)
- (void)setPrimaryBalance:(float)balance;

/// Set exclusive (hog) mode for an extra output device. Returns YES on success.
- (BOOL)setExclusive:(BOOL)exclusive forOutput:(int)sinkIndex;

/// Set 3-band parametric EQ gain for an extra output (band: 0=low 200Hz, 1=mid 1kHz, 2=high 5kHz; gain in dB, -12..+12)
- (void)setEQBand:(int)band gain:(float)gainDb forOutput:(int)sinkIndex;

/// Set 3-band parametric EQ gain for the primary output
- (void)setPrimaryEQBand:(int)band gain:(float)gainDb;

/// Set compressor on primary output (threshold dB, ratio, attack ms, release ms, enabled)
- (void)setPrimaryCompressorThreshold:(float)thresh ratio:(float)ratio attack:(float)attackMs release:(float)releaseMs enabled:(BOOL)enabled;

/// Set compressor on an extra output
- (void)setCompressorThreshold:(float)thresh ratio:(float)ratio attack:(float)attackMs release:(float)releaseMs enabled:(BOOL)enabled forOutput:(int)sinkIndex;

/// Set crossover filter on primary output (mode: 0=off, 1=LPF, 2=HPF; freq in Hz)
- (void)setPrimaryCrossoverMode:(int)mode frequency:(float)freqHz;

/// Set crossover filter on an extra output
- (void)setCrossoverMode:(int)mode frequency:(float)freqHz forOutput:(int)sinkIndex;

/// Set spatial audio on primary output (width: 0-2, crossfeed: 0-0.5)
- (void)setPrimarySpatialEnabled:(BOOL)enabled width:(float)width crossfeed:(float)crossfeed;

/// Set spatial audio on an extra output
- (void)setSpatialEnabled:(BOOL)enabled width:(float)width crossfeed:(float)crossfeed forOutput:(int)sinkIndex;

/// Set a callback to be notified when audio devices change (hot-plug)
- (void)setDeviceChangeCallback:(nullable void(^)(void))callback;

// ── Recording ──────────────────────────────────────────────────────────────

/// Start recording received audio to a WAV file at the given path.
/// Returns YES on success. Recording captures primary output audio.
- (BOOL)startRecordingToFile:(NSString *)path;

/// Stop recording and finalize the WAV file.
- (void)stopRecording;

/// Whether currently recording
@property (nonatomic, readonly) BOOL isRecording;

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

// ── WAN Relay (Group Code) ────────────────────────────────────────────────

/// WAN relay connection state
typedef NS_ENUM(NSInteger, SolunaRelayState) {
    SolunaRelayStateDisconnected = 0,
    SolunaRelayStateConnecting,
    SolunaRelayStateConnected,
    SolunaRelayStateError
};

/// Connect to a WAN relay server with a group code.
/// Returns YES if connection attempt started successfully.
- (BOOL)connectToRelay:(NSString *)host
                  port:(uint16_t)port
                 group:(NSString *)group
              password:(NSString *)password;

/// Disconnect from WAN relay
- (void)disconnectRelay;

/// Current WAN relay connection state
@property (nonatomic, readonly) SolunaRelayState relayState;

/// Currently connected group name (empty if not connected)
@property (nonatomic, readonly, copy, nullable) NSString *relayGroup;

/// Error message from last relay operation (nil if no error)
@property (nonatomic, readonly, copy, nullable) NSString *relayError;

// ── Mic Transmit (TX) ────────────────────────────────────────────────────

/// Whether mic transmit is currently active
@property (nonatomic, readonly) BOOL isMicTransmitting;

/// Number of TX packets sent
@property (nonatomic, readonly) uint64_t txPacketsSent;

/// Mic input peak level (0.0 - 1.0)
@property (nonatomic, readonly) float micInputLevel;

// ── Sync Mode ─────────────────────────────────────────────────────────────

/// Synchronized playback mode: all receivers play at the same wall-clock time.
/// When enabled, the receiver uses OSTP media_timestamp to compute the ideal
/// buffer depth so that audio on all devices exits the speaker at the same moment.
@property (nonatomic, assign) BOOL syncMode;

/// Target end-to-end delay in ms (50-1000, default 200). Only used when syncMode=YES.
@property (nonatomic, assign) uint32_t syncDelayMs;

// ── Mic Transmit (TX) ────────────────────────────────────────────────────

/// Start capturing from the microphone and transmitting via OSTP multicast.
/// Returns NO on failure.
- (BOOL)startMicTransmit;

/// Stop mic capture and transmission.
- (void)stopMicTransmit;

// ── System Audio Transmit (Soluna Virtual Device → Network) ─────────────

/// Whether system audio transmit is active (reads from Soluna.driver SHM)
@property (nonatomic, readonly) BOOL isShmTransmitting;

/// Number of system audio TX packets sent
@property (nonatomic, readonly) uint64_t shmTxPacketsSent;

/// System audio TX peak level (0.0 - 1.0)
@property (nonatomic, readonly) float shmTxLevel;

/// Start sending system audio from Soluna virtual device via OSTP.
/// Requires Soluna.driver to be installed. Returns NO on failure.
- (BOOL)startShmTransmit;

/// Stop system audio transmission.
- (void)stopShmTransmit;

@end

NS_ASSUME_NONNULL_END
