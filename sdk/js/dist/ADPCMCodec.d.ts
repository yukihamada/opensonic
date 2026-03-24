/**
 * IMA-ADPCM codec matching the C++ and Swift implementations.
 */
export interface ADPCMState {
    predicted: number;
    stepIndex: number;
}
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
export declare function decodeADPCMPayload(payload: Uint8Array): Int16Array | null;
/**
 * Decode raw ADPCM nibbles to Int16 PCM samples.
 * Each byte contains 2 nibbles (low nibble first).
 */
export declare function decodeADPCM(adpcm: Uint8Array, state: ADPCMState): Int16Array;
