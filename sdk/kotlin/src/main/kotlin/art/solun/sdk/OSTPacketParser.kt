package art.solun.sdk

/**
 * OSTP protocol constants matching the C++ implementation.
 */
object OSTConstants {
    /** RTP header extension profile for OSTP ("OS" = 0x4F53). */
    const val OSTP_PROFILE: Int = 0x4F53
    /** Default relay host. */
    const val DEFAULT_HOST: String = "relay.solun.art"
    /** Default relay port. */
    const val DEFAULT_PORT: Int = 5100
    /** Heartbeat interval in milliseconds. */
    const val HEARTBEAT_INTERVAL_MS: Long = 4000L
    /** Receive buffer size. */
    const val RECV_BUFFER_SIZE: Int = 16384
    /** RTP header size in bytes. */
    const val RTP_HEADER_SIZE: Int = 12
    /** CRC-32 trailer size in bytes. */
    const val CRC_TRAILER_SIZE: Int = 4
    /** ADPCM stereo payload type. */
    const val PT_ADPCM_STEREO: Int = 115
    /** ADPCM mono payload type. */
    const val PT_ADPCM_MONO: Int = 116
    /** Opus compressed payload type (RFC 6716). */
    const val PT_OPUS: Int = 98
    /** LC3 Bluetooth LE Audio payload type (liblc3, Apache 2.0). */
    const val PT_LC3: Int = 119
}

/**
 * A parsed OSTP/RTP audio packet.
 */
data class OSTPacket(
    /** RTP payload type. */
    val payloadType: Int,
    /** Number of audio channels (from stream_id bits [13:10]). */
    val channels: Int,
    /** Deck ID (from stream_id bits [15:14]). */
    val deckId: Int,
    /** Audio payload data (after header, before CRC trailer). */
    val payload: ByteArray,
    /** RTP sequence number. */
    val sequenceNumber: Int,
    /** RTP timestamp. */
    val timestamp: Long
)

/**
 * Parser for OSTP/RTP packets received from the Soluna relay.
 */
object OSTPacketParser {

    /**
     * Parse a raw UDP datagram as an OSTP/RTP audio packet.
     * Returns null for non-audio packets.
     */
    fun parse(data: ByteArray, length: Int = data.size): OSTPacket? {
        if (length < OSTConstants.RTP_HEADER_SIZE) return null

        // Check RTP version bits: (byte[0] & 0xC0) == 0x80
        if ((data[0].toInt() and 0xC0) != 0x80) return null

        val payloadType = data[1].toInt() and 0x7F
        val hasExtension = (data[0].toInt() and 0x10) != 0

        // Sequence number (big-endian, bytes 2-3)
        val sequenceNumber = ((data[2].toInt() and 0xFF) shl 8) or (data[3].toInt() and 0xFF)

        // Timestamp (big-endian, bytes 4-7)
        val timestamp = ((data[4].toLong() and 0xFF) shl 24) or
                ((data[5].toLong() and 0xFF) shl 16) or
                ((data[6].toLong() and 0xFF) shl 8) or
                (data[7].toLong() and 0xFF)

        var payloadOffset = OSTConstants.RTP_HEADER_SIZE
        var channels = 2 // default stereo
        var deckId = 0

        if (hasExtension && length >= 16) {
            val extProfile = ((data[12].toInt() and 0xFF) shl 8) or (data[13].toInt() and 0xFF)
            val extLenWords = ((data[14].toInt() and 0xFF) shl 8) or (data[15].toInt() and 0xFF)
            val extBytes = extLenWords * 4

            payloadOffset = OSTConstants.RTP_HEADER_SIZE + 4 + extBytes

            // OSTP: stream_id is first 2 bytes of extension data (byte 16-17)
            if (extProfile == OSTConstants.OSTP_PROFILE && length >= 18) {
                val streamId = ((data[16].toInt() and 0xFF) shl 8) or (data[17].toInt() and 0xFF)
                val ch = (streamId shr 10) and 0xF
                if (ch > 0) channels = ch
                deckId = (streamId shr 14) and 0x3
            }
        }

        // Strip CRC-32 trailer (last 4 bytes)
        var payloadEnd = length
        if (payloadEnd - payloadOffset > OSTConstants.CRC_TRAILER_SIZE) {
            payloadEnd -= OSTConstants.CRC_TRAILER_SIZE
        }

        if (payloadEnd <= payloadOffset) return null

        val payload = data.copyOfRange(payloadOffset, payloadEnd)

        return OSTPacket(
            payloadType = payloadType,
            channels = channels,
            deckId = deckId,
            payload = payload,
            sequenceNumber = sequenceNumber,
            timestamp = timestamp
        )
    }
}
