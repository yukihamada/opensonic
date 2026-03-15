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
 * Packet format (matching solunad/win-tx):
 *   RTP header (12B) + OstpHeader (20B) + int32 BE PCM + CRC-32 (4B)
 *
 * OstpHeader layout (all big-endian):
 *   magic[4]           = 'O','S','T','P'
 *   stream_id (uint32) = channel-count in upper 4 bits | id in lower 28
 *   seq_ext   (uint32) = upper 16 bits of sequence counter
 *   device_id (uint32) = SSRC (identifies this sender)
 *   flags     (uint32) = 0
 *   media_ts  (uint32) = media timestamp (sample count)
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
        val totalSize = 12 + 20 + payloadBytes + 4  // RTP + OstpHeader + payload + CRC

        val buf = ByteBuffer.allocate(totalSize).order(ByteOrder.BIG_ENDIAN)

        // RTP header (12 bytes)
        buf.put(0x80.toByte())                              // V=2, P=0, X=0, CC=0
        buf.put(PT_DYNAMIC.toByte())                        // M=0, PT=96
        buf.putShort((seqCounter and 0xFFFF).toShort())     // sequence
        buf.putInt((mediaTimestamp and 0xFFFFFFFFL).toInt()) // timestamp
        buf.putInt(ssrc)                                    // SSRC

        // OstpHeader (20 bytes): all big-endian
        //   magic[4]          = 'O','S','T','P'
        //   stream_id (u16)   = ch-count in upper 4 bits (ch << 12)
        //   seq_ext   (u16)   = 0
        //   device_id (u32)   = SSRC
        //   flags     (u32)   = 0
        //   media_ts  (u32)   = media timestamp
        buf.put('O'.code.toByte())
        buf.put('S'.code.toByte())
        buf.put('T'.code.toByte())
        buf.put('P'.code.toByte())
        buf.putShort((CHANNELS shl 12).toShort())           // stream_id: uint16, ch<<12
        buf.putShort(0.toShort())                           // seq_ext: uint16
        buf.putInt(ssrc)                                    // device_id
        buf.putInt(0)                                       // flags
        buf.putInt((mediaTimestamp and 0xFFFFFFFFL).toInt()) // media_timestamp

        // Payload: int32 big-endian PCM, scale = 2^23 (matches win build_ostp kSampleScale)
        for (i in 0 until sampleCount) {
            val s = (samples[i].coerceIn(-1f, 1f) * 8388608.0f).toInt()
            buf.putInt(s)
        }

        // CRC-32 over everything except the CRC field itself
        val bytes = buf.array()
        val crc = CRC32()
        crc.update(bytes, 0, 12 + 20 + payloadBytes)
        buf.putInt(crc.value.toInt())

        seqCounter++
        client.sendAudioPacketDirect(bytes)
    }
}
