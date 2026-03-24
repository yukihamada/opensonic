/**
 * Parser for OSTP/RTP packets received from the Soluna relay.
 */
/** OSTP protocol constants. */
export const OSTConstants = {
    /** RTP header extension profile for OSTP ("OS" = 0x4F53). */
    ostpProfile: 0x4F53,
    /** Default relay host. */
    defaultHost: "relay.solun.art",
    /** Default relay port. */
    defaultPort: 5100,
    /** Heartbeat interval in milliseconds. */
    heartbeatIntervalMs: 4000,
    /** Receive buffer size. */
    recvBufferSize: 16384,
    /** RTP header size in bytes. */
    rtpHeaderSize: 12,
    /** CRC-32 trailer size in bytes. */
    crcTrailerSize: 4,
    /** ADPCM stereo payload type. */
    ptADPCMStereo: 115,
    /** ADPCM mono payload type. */
    ptADPCMMono: 116,
};
/**
 * Parse a raw binary datagram as an OSTP/RTP audio packet.
 * Returns null for non-audio packets.
 */
export function parseOSTPacket(data) {
    if (data.length < OSTConstants.rtpHeaderSize)
        return null;
    // Check RTP version bits: (byte[0] & 0xC0) == 0x80
    if ((data[0] & 0xC0) !== 0x80)
        return null;
    const payloadType = data[1] & 0x7F;
    const hasExtension = (data[0] & 0x10) !== 0;
    // Sequence number (big-endian, bytes 2-3)
    const sequenceNumber = (data[2] << 8) | data[3];
    // Timestamp (big-endian, bytes 4-7)
    const timestamp = ((data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7]) >>> 0;
    let payloadOffset = OSTConstants.rtpHeaderSize;
    let channels = 2; // default stereo
    if (hasExtension && data.length >= 16) {
        // Extension header at byte 12-15
        const extProfile = (data[12] << 8) | data[13];
        const extLenWords = (data[14] << 8) | data[15];
        const extBytes = extLenWords * 4;
        payloadOffset = OSTConstants.rtpHeaderSize + 4 + extBytes;
        // OSTP: stream_id is first 2 bytes of extension data (byte 16-17)
        if (extProfile === OSTConstants.ostpProfile && data.length >= 18) {
            const streamId = (data[16] << 8) | data[17];
            const ch = (streamId >> 12) & 0xF;
            if (ch > 0)
                channels = ch;
        }
    }
    // Strip CRC-32 trailer (last 4 bytes)
    let payloadEnd = data.length;
    if (payloadEnd - payloadOffset > OSTConstants.crcTrailerSize) {
        payloadEnd -= OSTConstants.crcTrailerSize;
    }
    if (payloadEnd <= payloadOffset)
        return null;
    const payload = data.slice(payloadOffset, payloadEnd);
    return {
        payloadType,
        channels,
        payload,
        sequenceNumber,
        timestamp,
    };
}
