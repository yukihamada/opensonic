import Foundation

/// IMA-ADPCM codec matching the C++ and existing Swift implementations.
public enum ADPCMCodec {

    /// Standard IMA-ADPCM step size table (89 entries).
    public static let stepTable: [Int16] = [
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
        157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
        598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
        1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
        13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
    ]

    /// Standard IMA-ADPCM index adjustment table.
    public static let indexTable: [Int8] = [
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8,
    ]

    /// Decode IMA-ADPCM data to 16-bit PCM (little-endian byte pairs).
    ///
    /// The ADPCM payload has a 4-byte header:
    /// - [0-1]: predictor (int16 LE)
    /// - [2]: step index
    /// - [3]: reserved
    ///
    /// - Parameters:
    ///   - payload: Full ADPCM payload including the 4-byte header.
    /// - Returns: Decoded 16-bit PCM samples as raw bytes (little-endian int16 pairs), or nil on error.
    public static func decodePayload(_ payload: [UInt8]) -> [UInt8]? {
        guard payload.count >= 4 else { return nil }

        var state = ADPCMState(
            predicted: Int16(bitPattern: UInt16(payload[0]) | (UInt16(payload[1]) << 8)),
            stepIndex: min(payload[2], 88)
        )

        let adpcmData = Array(payload[4...])
        return decode(adpcm: adpcmData, state: &state)
    }

    /// Decode raw ADPCM nibbles to 16-bit PCM bytes.
    ///
    /// - Parameters:
    ///   - adpcm: Raw ADPCM data (each byte = 2 nibbles, low nibble first).
    ///   - state: ADPCM decoder state (modified in place).
    /// - Returns: Decoded PCM as little-endian int16 byte pairs.
    public static func decode(adpcm: [UInt8], state: inout ADPCMState) -> [UInt8] {
        let nSamples = adpcm.count * 2
        var out = [UInt8](repeating: 0, count: nSamples * 2)
        var outIdx = 0

        for byte in adpcm {
            for nibbleIdx in 0..<2 {
                let code: UInt8 = nibbleIdx == 0 ? (byte & 0x0F) : (byte >> 4)
                let step = Int32(stepTable[Int(state.stepIndex)])

                var delta = step >> 3
                if code & 4 != 0 { delta += step }
                if code & 2 != 0 { delta += step >> 1 }
                if code & 1 != 0 { delta += step >> 2 }
                if code & 8 != 0 { delta = -delta }

                let newPredicted = max(-32768, min(32767, Int32(state.predicted) + delta))
                state.predicted = Int16(newPredicted)

                let newIdx = max(0, min(88, Int8(state.stepIndex) + indexTable[Int(code)]))
                state.stepIndex = UInt8(newIdx)

                let leVal = state.predicted.littleEndian
                out[outIdx] = UInt8(UInt16(bitPattern: leVal) & 0xFF)
                out[outIdx + 1] = UInt8(UInt16(bitPattern: leVal) >> 8)
                outIdx += 2
            }
        }
        return out
    }
}
