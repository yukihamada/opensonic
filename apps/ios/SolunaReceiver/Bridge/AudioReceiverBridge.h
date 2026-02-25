//
//  AudioReceiverBridge.h
//  SolunaReceiver
//
//  Objective-C bridge for Swift interop with C++ audio receiver
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

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

@end

NS_ASSUME_NONNULL_END
