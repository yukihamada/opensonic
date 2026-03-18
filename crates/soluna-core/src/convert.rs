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
}
