package art.solun.sdk

/**
 * IMA-ADPCM codec matching the C++ and Swift implementations.
 */
object ADPCMCodec {

    /** Standard IMA-ADPCM step size table (89 entries). */
    val stepTable: ShortArray = shortArrayOf(
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
        157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
        598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
        1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
        13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
    )

    /** Standard IMA-ADPCM index adjustment table. */
    val indexTable: IntArray = intArrayOf(
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8
    )

    data class State(var predicted: Short = 0, var stepIndex: Int = 0)

    /**
     * Decode an ADPCM payload with 4-byte header to Int16 PCM samples.
     *
     * Header:
     * - [0-1]: predictor (int16 LE)
     * - [2]: step index
     * - [3]: reserved
     */
    fun decodePayload(payload: ByteArray): ShortArray? {
        if (payload.size < 4) return null

        val predicted = ((payload[0].toInt() and 0xFF) or
                ((payload[1].toInt() and 0xFF) shl 8)).toShort()
        val stepIndex = minOf(payload[2].toInt() and 0xFF, 88)
        val state = State(predicted, stepIndex)

        return decode(payload, 4, payload.size - 4, state)
    }

    /**
     * Decode raw ADPCM nibbles to Int16 PCM samples.
     * Each byte contains 2 nibbles (low nibble first).
     */
    fun decode(data: ByteArray, offset: Int, length: Int, state: State): ShortArray {
        val nSamples = length * 2
        val out = ShortArray(nSamples)
        var outIdx = 0

        for (i in offset until offset + length) {
            val byte = data[i].toInt() and 0xFF
            for (nibbleIdx in 0..1) {
                val code = if (nibbleIdx == 0) byte and 0x0F else byte shr 4
                val step = stepTable[state.stepIndex].toInt()

                var delta = step shr 3
                if (code and 4 != 0) delta += step
                if (code and 2 != 0) delta += step shr 1
                if (code and 1 != 0) delta += step shr 2
                if (code and 8 != 0) delta = -delta

                val newPredicted = (state.predicted.toInt() + delta).coerceIn(-32768, 32767)
                state.predicted = newPredicted.toShort()

                val newIdx = (state.stepIndex + indexTable[code]).coerceIn(0, 88)
                state.stepIndex = newIdx

                out[outIdx++] = state.predicted
            }
        }
        return out
    }
}
