package com.soluna.receiver

import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.util.Log
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.CRC32
import kotlin.math.abs

/**
 * Captures microphone audio and transmits OSTP packets to the relay via SolunaClient.
 *
 * Packet format (modern RTP extension, compatible with iOS and relay):
 *   RTP header (12B, X=1) + RTP ext header (4B) + OSTP ext (8B) + int32 BE PCM + CRC-32 (4B)
 *
 * RTP header byte 0: 0x90 (V=2, P=0, X=1, CC=0)
 * RTP ext header (all big-endian):
 *   profile (uint16) = 0x4F53 ("OS")
 *   length  (uint16) = 2       (2 × 32-bit words = 8 bytes)
 * OSTP ext (all big-endian):
 *   stream_id (uint16) = channel-count in upper 4 bits (ch << 12)
 *   seq_ext   (uint16) = upper 16 bits of sequence counter
 *   media_ts  (uint32) = RTP media timestamp (sample count)
 * CRC-32 covers payload only (not headers).
 */
class MicTransmitter(private val client: SolunaClient) {

    companion object {
        private const val TAG = "MicTransmitter"
        private const val SAMPLE_RATE = 48000
        private const val CHANNELS = 2
        private const val FRAMES_PER_PACKET = 480  // 10 ms @ 48 kHz
        private const val SAMPLES_PER_PACKET = FRAMES_PER_PACKET * CHANNELS
        private const val PT_DYNAMIC = 96
    }

    private val _level = MutableStateFlow(0f)
    val level: StateFlow<Float> = _level.asStateFlow()

    private var scope: CoroutineScope? = null
    private var seqCounter = 0
    private var mediaTimestamp = 0L
    private val ssrc: Int = (Math.random() * Int.MAX_VALUE).toInt()

    val isActive: Boolean get() = scope != null

    fun start() {
        if (scope != null) return
        scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
        scope?.launch { captureLoop() }
    }

    fun stop() {
        scope?.cancel()
        scope = null
        _level.value = 0f
    }

    private suspend fun captureLoop() {
        // Try stereo first, fall back to mono
        val (channelConfig, isMono) = tryCreateRecord()
        val readFrames = FRAMES_PER_PACKET
        val readSamples = if (isMono) readFrames else readFrames * CHANNELS
        val floatBuf = FloatArray(readSamples)

        val minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE, channelConfig, AudioFormat.ENCODING_PCM_FLOAT
        ).coerceAtLeast(readSamples * 4 * 4)

        val record = AudioRecord.Builder()
            .setAudioSource(MediaRecorder.AudioSource.MIC)
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(channelConfig)
                    .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                    .build()
            )
            .setBufferSizeInBytes(minBuf)
            .build()

        Log.i(TAG, "Mic TX started (${if (isMono) "mono→stereo" else "stereo"} 48kHz float → OSTP)")
        record.startRecording()

        try {
            while (currentCoroutineContext().isActive &&
                   record.recordingState == AudioRecord.RECORDSTATE_RECORDING) {

                val read = record.read(floatBuf, 0, readSamples, AudioRecord.READ_BLOCKING)
                if (read <= 0) continue

                // Compute mean-absolute level for UI meter
                var sum = 0f
                for (i in 0 until read) sum += abs(floatBuf[i])
                _level.value = sum / read

                // Upmix mono → stereo if needed
                val stereo = if (isMono) {
                    FloatArray(read * 2) { i -> floatBuf[i / 2] }
                } else {
                    floatBuf
                }
                val frameCount = if (isMono) read else read / 2

                buildAndSendPacket(stereo, frameCount * 2)
                mediaTimestamp += frameCount
            }
        } finally {
            record.stop()
            record.release()
            _level.value = 0f
            Log.i(TAG, "Mic TX stopped")
        }
    }

    private fun tryCreateRecord(): Pair<Int, Boolean> {
        val stereoMin = AudioRecord.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_FLOAT
        )
        return if (stereoMin > 0) {
            Pair(AudioFormat.CHANNEL_IN_STEREO, false)
        } else {
            Pair(AudioFormat.CHANNEL_IN_MONO, true)
        }
    }

    private fun buildAndSendPacket(samples: FloatArray, sampleCount: Int) {
        val payloadBytes = sampleCount * 4
        // Modern OSTP: RTP(12) + RTP ext header(4) + OSTP ext(8) + payload + CRC(4)
        val totalSize = 12 + 4 + 8 + payloadBytes + 4
        val payloadOffset = 24

        val buf = ByteBuffer.allocate(totalSize).order(ByteOrder.BIG_ENDIAN)

        // RTP header (12 bytes) — X=1 (extension present)
        buf.put(0x90.toByte())                                          // V=2, P=0, X=1, CC=0
        buf.put(PT_DYNAMIC.toByte())                                    // M=0, PT=96
        buf.putShort((seqCounter and 0xFFFF).toShort())                 // sequence (low 16 bits)
        buf.putInt((mediaTimestamp and 0xFFFFFFFFL).toInt())            // RTP timestamp
        buf.putInt(ssrc)                                                // SSRC

        // RTP extension header (4 bytes)
        buf.putShort(0x4F53.toShort())                                  // profile = "OS"
        buf.putShort(2.toShort())                                       // length = 2 words = 8 bytes

        // OSTP extension (8 bytes)
        buf.putShort((CHANNELS shl 12).toShort())                       // stream_id: ch << 12
        buf.putShort(((seqCounter shr 16) and 0xFFFF).toShort())        // seq_ext: upper 16 bits
        buf.putInt((mediaTimestamp and 0xFFFFFFFFL).toInt())            // media_ts

        // Payload: int32 big-endian PCM, scale = 2^23
        for (i in 0 until sampleCount) {
            val s = (samples[i].coerceIn(-1f, 1f) * 8388608.0f).toInt()
            buf.putInt(s)
        }

        // CRC-32 over payload only (matches relay's ostp_parse_packet and iOS)
        val bytes = buf.array()
        val crc = CRC32()
        crc.update(bytes, payloadOffset, payloadBytes)
        buf.putInt(crc.value.toInt())

        seqCounter++
        client.sendAudioPacketDirect(bytes)
    }
}
