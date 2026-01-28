#pragma once

#include <soluna/transport/rtp.h>
#include <soluna/soluna.h>
#include <cstdint>
#include <cstddef>

namespace soluna::transport {

/**
 * OSTP (Soluna Transport Protocol) extension header.
 * 8 bytes, carried as RTP header extension.
 *
 * Layout:
 *   stream_id      : 16 bits - unique stream identifier
 *   sequence_ext   : 16 bits - extended sequence (upper bits)
 *   media_timestamp: 32 bits - media clock timestamp (nanosecond-derived)
 */
struct __attribute__((packed)) OstpHeader {
    uint16_t stream_id;
    uint16_t sequence_ext;
    uint32_t media_timestamp;
};

static_assert(sizeof(OstpHeader) == 8, "OSTP header must be 8 bytes");

constexpr uint16_t kOstpProfile = 0x4F53; // "OS"

// Maximum packet sizes
constexpr size_t kRtpHeaderSize = sizeof(RtpHeader);
constexpr size_t kRtpExtHeaderSize = sizeof(RtpExtensionHeader);
constexpr size_t kOstpHeaderSize = sizeof(OstpHeader);
constexpr size_t kTotalHeaderSize = kRtpHeaderSize + kRtpExtHeaderSize + kOstpHeaderSize;

// Max payload: 48 samples * 64 channels * 4 bytes = 12288
// With headers: ~12312 bytes, well within jumbo frame
constexpr size_t kMaxPayloadSize = 12288;
constexpr size_t kMaxPacketSize = kTotalHeaderSize + kMaxPayloadSize;

/**
 * Build a complete OSTP/RTP packet.
 * Returns total packet size, or 0 on error.
 */
size_t ostp_build_packet(
    uint8_t* packet_buf,
    size_t buf_size,
    uint32_t ssrc,
    uint16_t sequence,
    uint32_t rtp_timestamp,
    uint8_t payload_type,
    uint16_t stream_id,
    uint16_t sequence_ext,
    uint32_t media_timestamp,
    const void* payload,
    size_t payload_size
);

/**
 * Parse an OSTP/RTP packet.
 * Returns true if valid, fills out header pointers.
 */
bool ostp_parse_packet(
    const uint8_t* packet_buf,
    size_t packet_size,
    RtpHeader& rtp,
    OstpHeader& ostp,
    const uint8_t*& payload,
    size_t& payload_size
);

} // namespace soluna::transport
