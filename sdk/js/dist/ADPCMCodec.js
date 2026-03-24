/**
 * IMA-ADPCM codec matching the C++ and Swift implementations.
 */
/** Standard IMA-ADPCM step size table (89 entries). */
const STEP_TABLE = new Int16Array([
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]);
/** Standard IMA-ADPCM index adjustment table. */
const INDEX_TABLE = new Int8Array([
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
]);
/**
 * Decode an ADPCM payload (with 4-byte header) to Int16 PCM samples.
 *
 * Header format:
 * - [0-1]: predictor (int16 LE)
 * - [2]: step index
 * - [3]: reserved
 *
 * Returns Int16Array of decoded PCM samples, or null on error.
 */
export function decodeADPCMPayload(payload) {
    if (payload.length < 4)
        return null;
    const state = {
        predicted: (payload[0] | (payload[1] << 8)) << 16 >> 16, // sign-extend int16
        stepIndex: Math.min(payload[2], 88),
    };
    const adpcmData = payload.subarray(4);
    return decodeADPCM(adpcmData, state);
}
/**
 * Decode raw ADPCM nibbles to Int16 PCM samples.
 * Each byte contains 2 nibbles (low nibble first).
 */
export function decodeADPCM(adpcm, state) {
    const nSamples = adpcm.length * 2;
    const out = new Int16Array(nSamples);
    let outIdx = 0;
    for (let i = 0; i < adpcm.length; i++) {
        const byte = adpcm[i];
        for (let nibbleIdx = 0; nibbleIdx < 2; nibbleIdx++) {
            const code = nibbleIdx === 0 ? (byte & 0x0F) : (byte >> 4);
            const step = STEP_TABLE[state.stepIndex];
            let delta = step >> 3;
            if (code & 4)
                delta += step;
            if (code & 2)
                delta += step >> 1;
            if (code & 1)
                delta += step >> 2;
            if (code & 8)
                delta = -delta;
            const newPredicted = Math.max(-32768, Math.min(32767, state.predicted + delta));
            state.predicted = newPredicted;
            const newIdx = Math.max(0, Math.min(88, state.stepIndex + INDEX_TABLE[code]));
            state.stepIndex = newIdx;
            out[outIdx++] = state.predicted;
        }
    }
    return out;
}
