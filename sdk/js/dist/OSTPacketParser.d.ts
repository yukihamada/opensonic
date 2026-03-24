/**
 * Parser for OSTP/RTP packets received from the Soluna relay.
 */
/** OSTP protocol constants. */
export declare const OSTConstants: {
    /** RTP header extension profile for OSTP ("OS" = 0x4F53). */
    readonly ostpProfile: 20307;
    /** Default relay host. */
    readonly defaultHost: "relay.solun.art";
    /** Default relay port. */
    readonly defaultPort: 5100;
    /** Heartbeat interval in milliseconds. */
    readonly heartbeatIntervalMs: 4000;
    /** Receive buffer size. */
    readonly recvBufferSize: 16384;
    /** RTP header size in bytes. */
    readonly rtpHeaderSize: 12;
    /** CRC-32 trailer size in bytes. */
    readonly crcTrailerSize: 4;
    /** ADPCM stereo payload type. */
    readonly ptADPCMStereo: 115;
    /** ADPCM mono payload type. */
    readonly ptADPCMMono: 116;
};
/** A parsed OSTP/RTP audio packet. */
export interface OSTPacket {
    /** RTP payload type. */
    payloadType: number;
    /** Number of audio channels (from stream_id upper 4 bits). */
    channels: number;
    /** Audio payload data (after header, before CRC trailer). */
    payload: Uint8Array;
    /** RTP sequence number. */
    sequenceNumber: number;
    /** RTP timestamp. */
    timestamp: number;
}
/**
 * Parse a raw binary datagram as an OSTP/RTP audio packet.
 * Returns null for non-audio packets.
 */
export declare function parseOSTPacket(data: Uint8Array): OSTPacket | null;
