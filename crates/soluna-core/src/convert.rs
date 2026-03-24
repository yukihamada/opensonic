/// Audio format conversion: S32LE / S24LE / S16LE / F32 → f32 normalized [-1.0, 1.0].

/// Convert interleaved S32LE (32-bit signed little-endian) samples to f32.
pub fn s32le_to_f32(data: &[u8], out: &mut Vec<f32>) {
    let sample_count = data.len() / 4;
    out.clear();
    out.reserve(sample_count);
    for i in 0..sample_count {
        let off = i * 4;
        let s = i32::from_le_bytes([data[off], data[off + 1], data[off + 2], data[off + 3]]);
        out.push(s as f32 / 2_147_483_648.0); // 2^31
    }
}

/// Convert interleaved S24LE (packed 3-byte) samples to f32.
pub fn s24le_to_f32(data: &[u8], out: &mut Vec<f32>) {
    let sample_count = data.len() / 3;
    out.clear();
    out.reserve(sample_count);
    for i in 0..sample_count {
        let off = i * 3;
        // Sign-extend 24-bit to 32-bit
        let raw = (data[off] as i32) | ((data[off + 1] as i32) << 8) | ((data[off + 2] as i32) << 16);
        let s = if raw & 0x80_0000 != 0 {
            raw | !0xFF_FFFF_i32 // sign extend
        } else {
            raw
        };
        out.push(s as f32 / 8_388_608.0); // 2^23
    }
}

/// Convert interleaved S16LE samples to f32.
pub fn s16le_to_f32(data: &[u8], out: &mut Vec<f32>) {
    let sample_count = data.len() / 2;
    out.clear();
    out.reserve(sample_count);
    for i in 0..sample_count {
        let off = i * 2;
        let s = i16::from_le_bytes([data[off], data[off + 1]]);
        out.push(s as f32 / 32768.0);
    }
}

/// Convert S24-in-S32LE (24-bit values in 32-bit little-endian containers) to f32.
/// This is the OSTP PT=96 format: each sample is 4 bytes, but values are in +/-8388607 range.
pub fn s24_in_s32le_to_f32(data: &[u8], out: &mut Vec<f32>) {
    let sample_count = data.len() / 4;
    out.clear();
    out.reserve(sample_count);
    for i in 0..sample_count {
        let off = i * 4;
        let s = i32::from_le_bytes([data[off], data[off + 1], data[off + 2], data[off + 3]]);
        out.push(s as f32 / 8_388_608.0); // 2^23, matching 24-bit range
    }
}

/// Convert interleaved F32LE samples to f32 (just reinterpret).
pub fn f32le_to_f32(data: &[u8], out: &mut Vec<f32>) {
    let sample_count = data.len() / 4;
    out.clear();
    out.reserve(sample_count);
    for i in 0..sample_count {
        let off = i * 4;
        let s = f32::from_le_bytes([data[off], data[off + 1], data[off + 2], data[off + 3]]);
        out.push(s);
    }
}

/// IMA-ADPCM step size table (89 entries).
const ADPCM_STEP_TABLE: [i32; 89] = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
];

/// IMA-ADPCM index adjustment table.
const ADPCM_INDEX_TABLE: [i32; 16] = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];

/// Decode IMA-ADPCM payload to f32 mono samples.
///
/// Payload format: [valprev:i16LE] [index:u8] [pad:u8] [nibbles...]
/// Each byte contains 2 nibbles (low nibble first), producing 2 samples per byte.
pub fn adpcm_to_f32(data: &[u8], out: &mut Vec<f32>) {
    out.clear();
    if data.len() < 4 {
        return;
    }
    let mut valprev = i16::from_le_bytes([data[0], data[1]]) as i32;
    let mut index = data[2] as i32;
    if index < 0 { index = 0; }
    if index > 88 { index = 88; }

    let nibble_bytes = data.len() - 4;
    out.reserve(nibble_bytes * 2);

    for bi in 0..nibble_bytes {
        let b = data[4 + bi];
        for half in 0..2u8 {
            let nib = if half == 0 { b & 0x0F } else { (b >> 4) & 0x0F } as i32;
            let step = ADPCM_STEP_TABLE[index as usize];
            let mut diff = step >> 3;
            if nib & 4 != 0 { diff += step; }
            if nib & 2 != 0 { diff += step >> 1; }
            if nib & 1 != 0 { diff += step >> 2; }
            if nib & 8 != 0 {
                valprev -= diff;
            } else {
                valprev += diff;
            }
            valprev = valprev.clamp(-32768, 32767);
            index += ADPCM_INDEX_TABLE[nib as usize];
            index = index.clamp(0, 88);
            out.push(valprev as f32 / 32768.0);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_s16le_roundtrip() {
        let raw: &[u8] = &[0x00, 0x40]; // 16384 in LE = 0.5
        let mut out = Vec::new();
        s16le_to_f32(raw, &mut out);
        assert!((out[0] - 0.5).abs() < 0.001);
    }

    #[test]
    fn test_s32le_zero() {
        let raw = [0u8; 8]; // two zero samples
        let mut out = Vec::new();
        s32le_to_f32(&raw, &mut out);
        assert_eq!(out.len(), 2);
        assert_eq!(out[0], 0.0);
    }

    #[test]
    fn test_adpcm_silence() {
        // ADPCM silence: valprev=0, index=0, all zero nibbles
        let mut data = vec![0u8; 4 + 48]; // 4-byte header + 48 nibble bytes = 96 samples
        // valprev=0 (LE), index=0, pad=0
        let mut out = Vec::new();
        adpcm_to_f32(&data, &mut out);
        assert_eq!(out.len(), 96);
        // All zero nibbles with step_table[0]=7: diff = 7>>3 = 0 (integer)
        // Actually diff = step>>3 = 7>>3 = 0, so valprev stays near 0
        for s in &out {
            assert!(s.abs() < 0.01, "expected near-silence, got {}", s);
        }
    }

    #[test]
    fn test_adpcm_encode_decode_roundtrip() {
        // Simulate what the C++ encoder does:
        // Encode a simple ramp, then decode and check it's reasonable
        let step_table = ADPCM_STEP_TABLE;
        let index_table = ADPCM_INDEX_TABLE;

        // Generate 96 PCM-16 samples: a simple sine-ish pattern
        let mut pcm16 = vec![0i16; 96];
        for i in 0..96 {
            pcm16[i] = (((i as f32 / 96.0) * std::f32::consts::TAU).sin() * 10000.0) as i16;
        }

        // Encode (matching C++ encoder)
        let mut valprev: i32 = 0;
        let mut index: i32 = 40;
        let mut adpcm_buf = vec![0u8; 4 + 48];
        adpcm_buf[0] = (valprev & 0xFF) as u8;
        adpcm_buf[1] = ((valprev >> 8) & 0xFF) as u8;
        adpcm_buf[2] = index as u8;
        adpcm_buf[3] = 0;

        for i in 0..96 {
            let sample = pcm16[i] as i32;
            let step = step_table[index as usize];
            let mut diff = sample - valprev;
            let mut nib: u8 = 0;
            if diff < 0 { nib |= 8; diff = -diff; }
            if diff >= step     { nib |= 4; diff -= step; }
            if diff >= step >> 1 { nib |= 2; diff -= step >> 1; }
            if diff >= step >> 2 { nib |= 1; }

            // Reconstruct (same as decoder)
            let mut rec_diff = step >> 3;
            if nib & 4 != 0 { rec_diff += step; }
            if nib & 2 != 0 { rec_diff += step >> 1; }
            if nib & 1 != 0 { rec_diff += step >> 2; }
            if nib & 8 != 0 { valprev -= rec_diff; } else { valprev += rec_diff; }
            valprev = valprev.clamp(-32768, 32767);
            index += index_table[nib as usize];
            index = index.clamp(0, 88);

            if i & 1 != 0 {
                adpcm_buf[4 + i / 2] |= nib << 4;
            } else {
                adpcm_buf[4 + i / 2] = nib;
            }
        }

        // Decode
        let mut out = Vec::new();
        adpcm_to_f32(&adpcm_buf, &mut out);
        assert_eq!(out.len(), 96);

        // Check output is in reasonable range and follows the sine pattern
        let max_val = out.iter().map(|s| s.abs()).fold(0.0f32, f32::max);
        assert!(max_val > 0.1, "output should have signal, max={}", max_val);
        assert!(max_val < 1.0, "output should not clip, max={}", max_val);
    }
}
