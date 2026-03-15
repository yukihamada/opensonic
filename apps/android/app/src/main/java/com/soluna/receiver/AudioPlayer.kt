package com.soluna.receiver

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.abs

/**
 * AudioTrack wrapper for OSTP audio playback.
 *
 * OSTP sends PCM as int32 samples (network byte order, big-endian).
 * Android AudioTrack supports PCM_FLOAT natively, so we convert
 * int32 -> float32 for playback at 48 kHz stereo.
 */
class AudioPlayer {

    companion object {
        private const val TAG = "AudioPlayer"
        const val SAMPLE_RATE = 48000
        const val CHANNELS = 2
        private const val INT32_MAX = 2147483647.0f
        private const val LEVEL_UPDATE_INTERVAL = SAMPLE_RATE / 10  // every 100ms
    }

    var onLevelUpdate: ((Float) -> Unit)? = null

    private var audioTrack: AudioTrack? = null
    private var isPlaying = false
    private var levelAccum = 0f
    private var levelCount = 0

    fun start() {
        if (isPlaying) return

        val bufferSize = AudioTrack.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_OUT_STEREO,
            AudioFormat.ENCODING_PCM_FLOAT
        ).coerceAtLeast(SAMPLE_RATE * CHANNELS * 4 / 10) // at least 100ms buffer

        audioTrack = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                    .build()
            )
            .setBufferSizeInBytes(bufferSize)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()

        audioTrack?.play()
        isPlaying = true
        Log.i(TAG, "AudioTrack started (48kHz stereo float32, buffer=$bufferSize)")
    }

    /**
     * Write OSTP audio payload to AudioTrack.
     *
     * Payload format: interleaved int32 samples, big-endian (network byte order).
     * Each sample is 4 bytes. Stereo = L,R,L,R,...
     *
     * We convert to float32 [-1.0, 1.0] for AudioTrack.
     */
    fun writePayload(payload: ByteArray, offset: Int, length: Int) {
        if (!isPlaying || audioTrack == null) return

        val sampleCount = length / 4
        if (sampleCount == 0) return

        val floatBuf = FloatArray(sampleCount)
        val bb = ByteBuffer.wrap(payload, offset, length).order(ByteOrder.BIG_ENDIAN)

        for (i in 0 until sampleCount) {
            val sample = bb.getInt()
            floatBuf[i] = sample / INT32_MAX
            levelAccum += abs(floatBuf[i])
        }
        levelCount += sampleCount
        if (levelCount >= LEVEL_UPDATE_INTERVAL) {
            onLevelUpdate?.invoke(levelAccum / levelCount)
            levelAccum = 0f
            levelCount = 0
        }

        audioTrack?.write(floatBuf, 0, sampleCount, AudioTrack.WRITE_NON_BLOCKING)
    }

    fun stop() {
        isPlaying = false
        try {
            audioTrack?.stop()
            audioTrack?.release()
        } catch (e: Exception) {
            Log.w(TAG, "Error stopping AudioTrack", e)
        }
        audioTrack = null
        Log.i(TAG, "AudioTrack stopped")
    }

    fun isActive(): Boolean = isPlaying
}
