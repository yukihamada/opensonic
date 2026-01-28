/**
 * PTPv2 Message Serialization / Deserialization
 *
 * Network byte order (big-endian) throughout.
 * SPDX-License-Identifier: MIT
 */

#include <soluna/sync/ptp.h>
#include <arpa/inet.h>
#include <cstring>

namespace soluna::sync {

// ---- Internal helpers ----

static void write_u8(uint8_t* buf, size_t& off, uint8_t val) {
    buf[off++] = val;
}

static void write_u16(uint8_t* buf, size_t& off, uint16_t val) {
    val = htons(val);
    std::memcpy(buf + off, &val, 2);
    off += 2;
}

static void write_u32(uint8_t* buf, size_t& off, uint32_t val) {
    val = htonl(val);
    std::memcpy(buf + off, &val, 4);
    off += 4;
}

static void write_i64(uint8_t* buf, size_t& off, int64_t val) {
    // Big-endian 64-bit
    uint64_t uval = static_cast<uint64_t>(val);
    for (int i = 7; i >= 0; i--) {
        buf[off + i] = static_cast<uint8_t>(uval & 0xFF);
        uval >>= 8;
    }
    off += 8;
}

static void write_timestamp(uint8_t* buf, size_t& off, const PtpTimestamp& ts) {
    write_u16(buf, off, ts.seconds_msb);
    write_u32(buf, off, ts.seconds_lsb);
    write_u32(buf, off, ts.nanoseconds);
}

static void write_port_identity(uint8_t* buf, size_t& off, const PtpPortIdentity& pid) {
    std::memcpy(buf + off, pid.clock_id.data(), 8);
    off += 8;
    write_u16(buf, off, pid.port_number);
}

static uint8_t read_u8(const uint8_t* buf, size_t& off) {
    return buf[off++];
}

static uint16_t read_u16(const uint8_t* buf, size_t& off) {
    uint16_t val;
    std::memcpy(&val, buf + off, 2);
    off += 2;
    return ntohs(val);
}

static uint32_t read_u32(const uint8_t* buf, size_t& off) {
    uint32_t val;
    std::memcpy(&val, buf + off, 4);
    off += 4;
    return ntohl(val);
}

static int64_t read_i64(const uint8_t* buf, size_t& off) {
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val = (val << 8) | buf[off + i];
    }
    off += 8;
    return static_cast<int64_t>(val);
}

static PtpTimestamp read_timestamp(const uint8_t* buf, size_t& off) {
    PtpTimestamp ts;
    ts.seconds_msb = read_u16(buf, off);
    ts.seconds_lsb = read_u32(buf, off);
    ts.nanoseconds = read_u32(buf, off);
    return ts;
}

static PtpPortIdentity read_port_identity(const uint8_t* buf, size_t& off) {
    PtpPortIdentity pid;
    std::memcpy(pid.clock_id.data(), buf + off, 8);
    off += 8;
    pid.port_number = read_u16(buf, off);
    return pid;
}

// ---- Serialize common header ----

static size_t serialize_header(uint8_t* buf, const PtpHeader& hdr) {
    size_t off = 0;

    // Byte 0: transport_specific (4 bits) | message_type (4 bits)
    write_u8(buf, off, static_cast<uint8_t>(hdr.message_type) & 0x0F);

    // Byte 1: version
    write_u8(buf, off, hdr.version & 0x0F);

    // Bytes 2-3: message length
    write_u16(buf, off, hdr.message_length);

    // Byte 4: domain
    write_u8(buf, off, hdr.domain_number);

    // Byte 5: reserved
    write_u8(buf, off, 0);

    // Bytes 6-7: flags
    buf[off++] = hdr.flags[0];
    buf[off++] = hdr.flags[1];

    // Bytes 8-15: correction field
    write_i64(buf, off, hdr.correction_field);

    // Bytes 16-19: reserved
    write_u32(buf, off, 0);

    // Bytes 20-29: source port identity
    write_port_identity(buf, off, hdr.source_port_id);

    // Bytes 30-31: sequence id
    write_u16(buf, off, hdr.sequence_id);

    // Byte 32: control field
    write_u8(buf, off, hdr.control_field);

    // Byte 33: log message interval
    write_u8(buf, off, static_cast<uint8_t>(hdr.log_message_interval));

    return off; // should be 34
}

// ---- Public serialization functions ----

size_t ptp_serialize_sync(uint8_t* buf, size_t buf_size,
    const PtpHeader& hdr, const PtpTimestamp& origin_ts)
{
    if (buf_size < kPtpSyncSize) return 0;

    PtpHeader h = hdr;
    h.message_type = PtpMessageType::Sync;
    h.message_length = kPtpSyncSize;
    h.control_field = 0x00; // Sync

    size_t off = serialize_header(buf, h);
    write_timestamp(buf, off, origin_ts);
    return off;
}

size_t ptp_serialize_follow_up(uint8_t* buf, size_t buf_size,
    const PtpHeader& hdr, const PtpTimestamp& precise_ts)
{
    if (buf_size < kPtpFollowUpSize) return 0;

    PtpHeader h = hdr;
    h.message_type = PtpMessageType::FollowUp;
    h.message_length = kPtpFollowUpSize;
    h.control_field = 0x02; // Follow_Up

    size_t off = serialize_header(buf, h);
    write_timestamp(buf, off, precise_ts);
    return off;
}

size_t ptp_serialize_delay_req(uint8_t* buf, size_t buf_size,
    const PtpHeader& hdr, const PtpTimestamp& origin_ts)
{
    if (buf_size < kPtpDelayReqSize) return 0;

    PtpHeader h = hdr;
    h.message_type = PtpMessageType::DelayReq;
    h.message_length = kPtpDelayReqSize;
    h.control_field = 0x01; // Delay_Req

    size_t off = serialize_header(buf, h);
    write_timestamp(buf, off, origin_ts);
    return off;
}

size_t ptp_serialize_delay_resp(uint8_t* buf, size_t buf_size,
    const PtpHeader& hdr, const PtpDelayRespBody& body)
{
    if (buf_size < kPtpDelayRespSize) return 0;

    PtpHeader h = hdr;
    h.message_type = PtpMessageType::DelayResp;
    h.message_length = kPtpDelayRespSize;
    h.control_field = 0x03; // Delay_Resp

    size_t off = serialize_header(buf, h);
    write_timestamp(buf, off, body.receive_timestamp);
    write_port_identity(buf, off, body.requesting_port_id);
    return off;
}

size_t ptp_serialize_announce(uint8_t* buf, size_t buf_size,
    const PtpHeader& hdr, const PtpAnnounceBody& body)
{
    if (buf_size < kPtpAnnounceSize) return 0;

    PtpHeader h = hdr;
    h.message_type = PtpMessageType::Announce;
    h.message_length = kPtpAnnounceSize;
    h.control_field = 0x05; // Announce

    size_t off = serialize_header(buf, h);

    // Origin timestamp
    write_timestamp(buf, off, body.origin_timestamp);

    // Current UTC offset
    write_u16(buf, off, body.current_utc_offset);

    // Reserved byte
    write_u8(buf, off, 0);

    // Grandmaster priority1
    write_u8(buf, off, body.grandmaster_priority1);

    // Grandmaster clock quality
    write_u8(buf, off, static_cast<uint8_t>(body.grandmaster_clock_quality.clock_class));
    write_u8(buf, off, static_cast<uint8_t>(body.grandmaster_clock_quality.clock_accuracy));
    write_u16(buf, off, body.grandmaster_clock_quality.offset_scaled_log_variance);

    // Grandmaster priority2
    write_u8(buf, off, body.grandmaster_priority2);

    // Grandmaster identity
    std::memcpy(buf + off, body.grandmaster_identity.data(), 8);
    off += 8;

    // Steps removed
    write_u16(buf, off, body.steps_removed);

    // Time source
    write_u8(buf, off, body.time_source);

    return off;
}

// ---- Public deserialization functions ----

bool ptp_parse_header(const uint8_t* buf, size_t buf_size, PtpHeader& hdr) {
    if (buf_size < kPtpHeaderSize) return false;

    size_t off = 0;

    uint8_t byte0 = read_u8(buf, off);
    hdr.message_type = static_cast<PtpMessageType>(byte0 & 0x0F);

    uint8_t byte1 = read_u8(buf, off);
    hdr.version = byte1 & 0x0F;
    if (hdr.version != 2) return false;

    hdr.message_length = read_u16(buf, off);
    hdr.domain_number = read_u8(buf, off);

    off++; // reserved

    hdr.flags[0] = buf[off++];
    hdr.flags[1] = buf[off++];

    hdr.correction_field = read_i64(buf, off);

    off += 4; // reserved

    hdr.source_port_id = read_port_identity(buf, off);
    hdr.sequence_id = read_u16(buf, off);
    hdr.control_field = read_u8(buf, off);
    hdr.log_message_interval = static_cast<int8_t>(read_u8(buf, off));

    return true;
}

bool ptp_parse_timestamp_body(const uint8_t* buf, size_t buf_size, PtpTimestamp& ts) {
    if (buf_size < kPtpSyncSize) return false;
    size_t off = kPtpHeaderSize;
    ts = read_timestamp(buf, off);
    return true;
}

bool ptp_parse_delay_resp(const uint8_t* buf, size_t buf_size, PtpDelayRespBody& body) {
    if (buf_size < kPtpDelayRespSize) return false;
    size_t off = kPtpHeaderSize;
    body.receive_timestamp = read_timestamp(buf, off);
    body.requesting_port_id = read_port_identity(buf, off);
    return true;
}

bool ptp_parse_announce(const uint8_t* buf, size_t buf_size, PtpAnnounceBody& body) {
    if (buf_size < kPtpAnnounceSize) return false;
    size_t off = kPtpHeaderSize;

    body.origin_timestamp = read_timestamp(buf, off);
    body.current_utc_offset = read_u16(buf, off);

    off++; // reserved

    body.grandmaster_priority1 = read_u8(buf, off);

    body.grandmaster_clock_quality.clock_class = static_cast<PtpClockClass>(read_u8(buf, off));
    body.grandmaster_clock_quality.clock_accuracy = static_cast<PtpClockAccuracy>(read_u8(buf, off));
    body.grandmaster_clock_quality.offset_scaled_log_variance = read_u16(buf, off);

    body.grandmaster_priority2 = read_u8(buf, off);

    std::memcpy(body.grandmaster_identity.data(), buf + off, 8);
    off += 8;

    body.steps_removed = read_u16(buf, off);
    body.time_source = read_u8(buf, off);

    return true;
}

} // namespace soluna::sync
