//! IMA-ADPCM codec matching the C++ and Swift implementations.

/// Standard IMA-ADPCM step size table (89 entries).
pub const STEP_TABLE: [i16; 89] = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
];

/// Standard IMA-ADPCM index adjustment table.
pub const INDEX_TABLE: [i8; 16] = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];

/// ADPCM decoder state.
#[derive(Debug, Clone, Copy)]
pub struct ADPCMState {
    pub predicted: i16,
    pub step_index: u8,
}

impl Default for ADPCMState {
    fn default() -> Self {
        Self {
            predicted: 0,
            step_index: 0,
        }
    }
}

impl ADPCMState {
    pub fn new(predicted: i16, step_index: u8) -> Self {
        Self {
            predicted,
            step_index: step_index.min(88),
        }
    }
}

/// Decode an ADPCM payload with 4-byte header to i16 PCM samples.
///
/// Header:
/// - [0-1]: predictor (int16 LE)
/// - [2]: step index
/// - [3]: reserved
pub fn decode_adpcm_payload(payload: &[u8]) -> Option<Vec<i16>> {
    if payload.len() < 4 {
        return None;
    }

    let predicted = i16::from_le_bytes([payload[0], payload[1]]);
    let step_index = payload[2].min(88);
    let mut state = ADPCMState::new(predicted, step_index);

    Some(decode_adpcm(&payload[4..], &mut state))
}

/// Decode raw ADPCM nibbles to i16 PCM samples.
/// Each byte contains 2 nibbles (low nibble first).
pub fn decode_adpcm(adpcm: &[u8], state: &mut ADPCMState) -> Vec<i16> {
    let n_samples = adpcm.len() * 2;
    let mut out = Vec::with_capacity(n_samples);

    for &byte in adpcm {
        for nibble_idx in 0..2 {
            let code = if nibble_idx == 0 {
                byte & 0x0F
            } else {
                byte >> 4
            };
            let step = STEP_TABLE[state.step_index as usize] as i32;

            let mut delta = step >> 3;
            if code & 4 != 0 {
                delta += step;
            }
            if code & 2 != 0 {
                delta += step >> 1;
            }
            if code & 1 != 0 {
                delta += step >> 2;
            }
            if code & 8 != 0 {
                delta = -delta;
            }

            let new_predicted = (state.predicted as i32 + delta).clamp(-32768, 32767);
            state.predicted = new_predicted as i16;

            let new_idx =
                (state.step_index as i8 + INDEX_TABLE[code as usize]).clamp(0, 88) as u8;
            state.step_index = new_idx;

            out.push(state.predicted);
        }
    }
    out
}
