/// Downmix N-channel interleaved audio to M-channel output.
///
/// Supports any combination: mono output (average all), stereo from surround, pass-through.

/// Downmix one frame of `in_ch` channels to `out_ch` channels.
///
/// `src` must contain exactly `in_ch` samples (one frame).
/// `dst` must be at least `out_ch` samples long.
pub fn downmix_frame(src: &[f32], in_ch: usize, dst: &mut [f32], out_ch: usize) {
    if in_ch == 0 || out_ch == 0 {
        return;
    }

    if in_ch == out_ch {
        // Pass-through
        dst[..out_ch].copy_from_slice(&src[..out_ch]);
        return;
    }

    if out_ch == 1 {
        // Mono output: average all input channels
        let sum: f32 = src[..in_ch].iter().sum();
        dst[0] = sum / in_ch as f32;
        return;
    }

    if out_ch == 2 {
        // Stereo output from N channels
        match in_ch {
            1 => {
                // Mono → Stereo: duplicate
                dst[0] = src[0];
                dst[1] = src[0];
            }
            6 => {
                // 5.1 → Stereo (ITU-R BS.775)
                // L' = L + 0.707*C + 0.707*Ls
                // R' = R + 0.707*C + 0.707*Rs
                // 5.1 layout: FL, FR, C, LFE, SL, SR
                let fl = src[0];
                let fr = src[1];
                let c = src[2];
                let _lfe = src[3];
                let sl = src[4];
                let sr = src[5];
                dst[0] = fl + 0.707 * c + 0.707 * sl;
                dst[1] = fr + 0.707 * c + 0.707 * sr;
            }
            8 => {
                // 7.1 → Stereo
                // Layout: FL, FR, C, LFE, SL, SR, BL, BR
                let fl = src[0];
                let fr = src[1];
                let c = src[2];
                let _lfe = src[3];
                let sl = src[4];
                let sr = src[5];
                let bl = src[6];
                let br = src[7];
                dst[0] = fl + 0.707 * c + 0.5 * sl + 0.5 * bl;
                dst[1] = fr + 0.707 * c + 0.5 * sr + 0.5 * br;
            }
            _ => {
                // Generic: even-indexed → L, odd-indexed → R
                let mut l = 0.0f32;
                let mut r = 0.0f32;
                for (i, &s) in src[..in_ch].iter().enumerate() {
                    if i % 2 == 0 {
                        l += s;
                    } else {
                        r += s;
                    }
                }
                let half = (in_ch as f32 / 2.0).max(1.0);
                dst[0] = l / half;
                dst[1] = r / half;
            }
        }
        return;
    }

    // out_ch > 2 and in_ch != out_ch: fill what we can, zero the rest
    let copy = in_ch.min(out_ch);
    dst[..copy].copy_from_slice(&src[..copy]);
    for s in dst[copy..out_ch].iter_mut() {
        *s = 0.0;
    }
}

/// Downmix an entire buffer of interleaved audio.
///
/// `src` contains `frame_count * in_ch` samples.
/// Returns a new buffer of `frame_count * out_ch` samples.
pub fn downmix_buffer(src: &[f32], in_ch: usize, out_ch: usize) -> Vec<f32> {
    if in_ch == 0 || out_ch == 0 {
        return Vec::new();
    }
    let frame_count = src.len() / in_ch;
    let mut dst = vec![0.0f32; frame_count * out_ch];

    for f in 0..frame_count {
        let src_off = f * in_ch;
        let dst_off = f * out_ch;
        downmix_frame(
            &src[src_off..src_off + in_ch],
            in_ch,
            &mut dst[dst_off..dst_off + out_ch],
            out_ch,
        );
    }
    dst
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_stereo_to_mono() {
        let src = [0.5, -0.5]; // L=0.5, R=-0.5
        let mut dst = [0.0f32; 1];
        downmix_frame(&src, 2, &mut dst, 1);
        assert!((dst[0] - 0.0).abs() < 0.001);
    }

    #[test]
    fn test_mono_to_stereo() {
        let src = [0.8];
        let mut dst = [0.0f32; 2];
        downmix_frame(&src, 1, &mut dst, 2);
        assert_eq!(dst[0], 0.8);
        assert_eq!(dst[1], 0.8);
    }

    #[test]
    fn test_passthrough() {
        let src = [0.1, 0.2];
        let mut dst = [0.0f32; 2];
        downmix_frame(&src, 2, &mut dst, 2);
        assert_eq!(dst, src);
    }
}
