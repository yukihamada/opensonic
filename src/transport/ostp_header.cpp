#include <soluna/transport/ostp.h>
#include <cstring>
#include <arpa/inet.h>

namespace soluna::transport {

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
    size_t payload_size)
{
    const size_t total = kTotalHeaderSize + payload_size;
    if (buf_size < total) return 0;

    // RTP header
    auto* rtp = reinterpret_cast<RtpHeader*>(packet_buf);
    std::memset(rtp, 0, sizeof(RtpHeader));
    rtp->version = 2;
    rtp->extension = 1; // OSTP extension present
    rtp->pt = payload_type;
    rtp->sequence = htons(sequence);
    rtp->timestamp = htonl(rtp_timestamp);
    rtp->ssrc = htonl(ssrc);

    // RTP extension header
    auto* ext = reinterpret_cast<RtpExtensionHeader*>(packet_buf + kRtpHeaderSize);
    ext->profile_specific = htons(kOstpProfile);
    ext->length = htons(2); // 2 x 32-bit words = 8 bytes

    // OSTP header
    auto* ostp = reinterpret_cast<OstpHeader*>(packet_buf + kRtpHeaderSize + kRtpExtHeaderSize);
    ostp->stream_id = htons(stream_id);
    ostp->sequence_ext = htons(sequence_ext);
    ostp->media_timestamp = htonl(media_timestamp);

    // Payload
    if (payload && payload_size > 0) {
        std::memcpy(packet_buf + kTotalHeaderSize, payload, payload_size);
    }

    return total;
}

bool ostp_parse_packet(
    const uint8_t* packet_buf,
    size_t packet_size,
    RtpHeader& rtp,
    OstpHeader& ostp,
    const uint8_t*& payload,
    size_t& payload_size)
{
    if (packet_size < kTotalHeaderSize) return false;

    // Parse RTP header
    const auto* rtp_raw = reinterpret_cast<const RtpHeader*>(packet_buf);
    if (rtp_raw->version != 2) return false;

    rtp = *rtp_raw;
    rtp.sequence = ntohs(rtp.sequence);
    rtp.timestamp = ntohl(rtp.timestamp);
    rtp.ssrc = ntohl(rtp.ssrc);

    // Check extension
    if (!rtp.extension) return false;

    const auto* ext = reinterpret_cast<const RtpExtensionHeader*>(packet_buf + kRtpHeaderSize);
    if (ntohs(ext->profile_specific) != kOstpProfile) return false;
    if (ntohs(ext->length) != 2) return false;

    // Parse OSTP header
    const auto* ostp_raw = reinterpret_cast<const OstpHeader*>(
        packet_buf + kRtpHeaderSize + kRtpExtHeaderSize);
    ostp.stream_id = ntohs(ostp_raw->stream_id);
    ostp.sequence_ext = ntohs(ostp_raw->sequence_ext);
    ostp.media_timestamp = ntohl(ostp_raw->media_timestamp);

    payload = packet_buf + kTotalHeaderSize;
    payload_size = packet_size - kTotalHeaderSize;

    return true;
}

} // namespace soluna::transport
