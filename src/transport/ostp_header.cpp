#include <soluna/transport/ostp.h>
#include <cstring>
#include <arpa/inet.h>

namespace soluna::transport {

// CRC-32 (IEEE 802.3 polynomial, bit-at-a-time)
uint32_t ostp_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

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
    const size_t total = kTotalHeaderSize + payload_size + kCrcTrailerSize;
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

    // CRC-32 trailer (over payload only, network byte order)
    uint32_t crc = ostp_crc32(packet_buf + kTotalHeaderSize, payload_size);
    uint32_t crc_be = htonl(crc);
    std::memcpy(packet_buf + kTotalHeaderSize + payload_size, &crc_be, 4);

    return total;
}

int ostp_parse_packet(
    const uint8_t* packet_buf,
    size_t packet_size,
    RtpHeader& rtp,
    OstpHeader& ostp,
    const uint8_t*& payload,
    size_t& payload_size)
{
    if (packet_size < kTotalHeaderSize) return -1;

    // Parse RTP header
    const auto* rtp_raw = reinterpret_cast<const RtpHeader*>(packet_buf);
    if (rtp_raw->version != 2) return -1;

    rtp = *rtp_raw;
    rtp.sequence = ntohs(rtp.sequence);
    rtp.timestamp = ntohl(rtp.timestamp);
    rtp.ssrc = ntohl(rtp.ssrc);

    // Check extension
    if (!rtp.extension) return -1;

    const auto* ext = reinterpret_cast<const RtpExtensionHeader*>(packet_buf + kRtpHeaderSize);
    if (ntohs(ext->profile_specific) != kOstpProfile) return -1;
    if (ntohs(ext->length) != 2) return -1;

    // Parse OSTP header
    const auto* ostp_raw = reinterpret_cast<const OstpHeader*>(
        packet_buf + kRtpHeaderSize + kRtpExtHeaderSize);
    ostp.stream_id = ntohs(ostp_raw->stream_id);
    ostp.sequence_ext = ntohs(ostp_raw->sequence_ext);
    ostp.media_timestamp = ntohl(ostp_raw->media_timestamp);

    // Determine if CRC-32 trailer is present.
    // New packets: payload_size is a multiple of frame_size, with 4 extra bytes for CRC.
    // Backward compat: if (data_len - 4) is NOT a valid payload (not multiple of 4),
    // treat the whole data as payload (no CRC).
    size_t data_len = packet_size - kTotalHeaderSize;

    if (data_len >= kCrcTrailerSize) {
        size_t candidate_payload = data_len - kCrcTrailerSize;
        // Payload must be a multiple of sizeof(int32_t) (one sample = 4 bytes)
        if (candidate_payload % sizeof(int32_t) == 0) {
            // CRC present — verify
            payload = packet_buf + kTotalHeaderSize;
            payload_size = candidate_payload;

            uint32_t recv_crc;
            std::memcpy(&recv_crc, packet_buf + kTotalHeaderSize + payload_size, 4);
            recv_crc = ntohl(recv_crc);

            uint32_t calc_crc = ostp_crc32(payload, payload_size);
            if (recv_crc == calc_crc) return 0; // CRC valid
            // CRC mismatch — fall through to no-CRC path (radio bots don't add CRC)
        }
    }

    // No CRC trailer (old-format packet or payload not aligned)
    payload = packet_buf + kTotalHeaderSize;
    payload_size = data_len;
    return 0;
}

} // namespace soluna::transport
