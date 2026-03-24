package art.solun.sdk

/**
 * Audio playback using Android AudioTrack.
 *
 * Note: This requires the Android SDK. On non-Android JVM platforms,
 * use the decoded float32 samples directly via the on_audio callback.
 *
 * This file provides the decoding logic that works on any JVM.
 * Android-specific AudioTrack usage is shown in comments.
 */
object AudioDecoder {

    /**
     * Decode a packet's payload to interleaved float32 PCM samples.
     */
    fun decodePacketToFloat(packet: OSTPacket): FloatArray? {
        return when (packet.payloadType) {
            OSTConstants.PT_ADPCM_STEREO, OSTConstants.PT_ADPCM_MONO ->
                decodeADPCMToFloat(packet.payload)
            OSTConstants.PT_OPUS ->
                decodeOpusToFloat(packet.payload, packet.channels)
            OSTConstants.PT_LC3 ->
                decodeLC3ToFloat(packet.payload, packet.channels)
            else ->
                decodeInt32LEToFloat(packet.payload)
        }
    }

    /**
     * Decode int32 LE interleaved payload to float32.
     */
    fun decodeInt32LEToFloat(payload: ByteArray): FloatArray? {
        val totalSamples = payload.size / 4
        if (totalSamples == 0) return null

        val scale = 1.0f / Int.MAX_VALUE.toFloat()
        val out = FloatArray(totalSamples)

        for (i in 0 until totalSamples) {
            val offset = i * 4
            if (offset + 3 >= payload.size) break
            val value = (payload[offset].toInt() and 0xFF) or
                    ((payload[offset + 1].toInt() and 0xFF) shl 8) or
                    ((payload[offset + 2].toInt() and 0xFF) shl 16) or
                    ((payload[offset + 3].toInt() and 0xFF) shl 24)
            out[i] = value.toFloat() * scale
        }
        return out
    }

    /**
     * Decode Opus payload to float32 using Android MediaCodec.
     *
     * On Android, use MediaCodec with MIMETYPE_AUDIO_OPUS for hardware-accelerated decode.
     * On non-Android JVM, this is a stub.
     *
     * TODO: Implement MediaCodec-based Opus decode for Android targets.
     */
    fun decodeOpusToFloat(payload: ByteArray, channels: Int): FloatArray? {
        // Android implementation would use:
        //   val codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS)
        //   val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, 48000, channels)
        //   codec.configure(format, null, null, 0)
        //   codec.start()
        //   // ... queue input buffer, dequeue output buffer ...
        println("[SolunaSDK] Opus decode via MediaCodec not yet implemented on this platform")
        return null
    }

    /**
     * Decode LC3 payload to float32.
     *
     * LC3 decode requires liblc3 (Google, Apache 2.0).
     * https://github.com/google/liblc3
     * TODO: Add JNI bindings for liblc3.
     */
    fun decodeLC3ToFloat(payload: ByteArray, channels: Int): FloatArray? {
        println("[SolunaSDK] LC3 codec not yet supported — install liblc3")
        return null
    }

    /**
     * Decode ADPCM payload to float32.
     */
    fun decodeADPCMToFloat(payload: ByteArray): FloatArray? {
        val pcm16 = ADPCMCodec.decodePayload(payload) ?: return null
        return FloatArray(pcm16.size) { pcm16[it].toFloat() / 32768.0f }
    }

    /**
     * Convert interleaved samples to stereo.
     * Mono is duplicated, multi-channel takes first 2.
     */
    fun toStereo(samples: FloatArray, channels: Int): FloatArray {
        if (channels == 2) return samples

        val frames = samples.size / maxOf(channels, 1)
        val out = FloatArray(frames * 2)

        if (channels == 1) {
            for (f in 0 until frames) {
                out[f * 2] = samples[f]
                out[f * 2 + 1] = samples[f]
            }
        } else {
            for (f in 0 until frames) {
                val base = f * channels
                out[f * 2] = if (base < samples.size) samples[base] else 0f
                out[f * 2 + 1] = if (base + 1 < samples.size) samples[base + 1] else 0f
            }
        }
        return out
    }
}

/*
 * Android AudioTrack usage example:
 *
 * import android.media.AudioAttributes
 * import android.media.AudioFormat
 * import android.media.AudioTrack
 *
 * class AndroidAudioPlayer {
 *     private var audioTrack: AudioTrack? = null
 *
 *     fun start() {
 *         val bufferSize = AudioTrack.getMinBufferSize(
 *             48000,
 *             AudioFormat.CHANNEL_OUT_STEREO,
 *             AudioFormat.ENCODING_PCM_FLOAT
 *         )
 *         audioTrack = AudioTrack.Builder()
 *             .setAudioAttributes(AudioAttributes.Builder()
 *                 .setUsage(AudioAttributes.USAGE_MEDIA)
 *                 .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
 *                 .build())
 *             .setAudioFormat(AudioFormat.Builder()
 *                 .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
 *                 .setSampleRate(48000)
 *                 .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
 *                 .build())
 *             .setBufferSizeInBytes(bufferSize)
 *             .setTransferMode(AudioTrack.MODE_STREAM)
 *             .build()
 *         audioTrack?.play()
 *     }
 *
 *     fun playPacket(packet: OSTPacket) {
 *         val samples = AudioDecoder.decodePacketToFloat(packet) ?: return
 *         val stereo = AudioDecoder.toStereo(samples, packet.channels)
 *         audioTrack?.write(stereo, 0, stereo.size, AudioTrack.WRITE_NON_BLOCKING)
 *     }
 *
 *     fun stop() {
 *         audioTrack?.stop()
 *         audioTrack?.release()
 *         audioTrack = null
 *     }
 * }
 */
