/// Opus codec support for Soluna.
///
/// Since no pure-Rust Opus decoder compiles to wasm32-unknown-unknown,
/// this module provides:
///
/// 1. **Packet validation** — parse Opus TOC byte, extract frame count/duration
/// 2. **Silence generator** — produce silence frames matching the expected Opus frame size
/// 3. **F32 PCM passthrough** — accept pre-decoded PCM from the JS WebCodecs bridge
///
/// Actual Opus encode/decode is handled in JavaScript via the WebCodecs API
/// (`AudioDecoder` / `AudioEncoder`), which supports Opus natively in modern browsers.
///
/// For native (non-wasm) builds, the `opus` feature can gate in `audiopus_sys` bindings.

/// Opus frame durations in microseconds indexed by TOC config (0..31).
/// From RFC 6716 Table 2.
const FRAME_DURATIONS_US: [u32; 32] = [
    // SILK-only: NB
    10000, 20000, 40000, 60000,
    // SILK-only: MB
    10000, 20000, 40000, 60000,
    // SILK-only: WB
    10000, 20000, 40000, 60000,
    // Hybrid: SWB
    10000, 20000,
    // Hybrid: FB
    10000, 20000,
    // CELT-only: NB
    2500, 5000, 10000, 20000,
    // CELT-only: MB
    2500, 5000, 10000, 20000,
    // CELT-only: WB
    2500, 5000, 10000, 20000,
    // CELT-only: SWB
    2500, 5000, 10000, 20000,
];

/// Parsed Opus TOC (Table of Contents) byte.
#[derive(Debug, Clone, Copy)]
pub struct OpusToc {
    /// Configuration number (0..31) — determines bandwidth and frame size.
    pub config: u8,
    /// Stereo flag: true if stereo, false if mono.
    pub stereo: bool,
    /// Frame count code (0..3):
    ///   0 = 1 frame
    ///   1 = 2 frames, equal size
    ///   2 = 2 frames, different sizes
    ///   3 = arbitrary number of frames (CBR or VBR)
    pub frame_count_code: u8,
}

/// Information extracted from an Opus packet header.
#[derive(Debug, Clone, Copy)]
pub struct OpusPacketInfo {
    pub toc: OpusToc,
    /// Number of audio frames in this packet.
    pub frame_count: u32,
    /// Duration of each frame in microseconds.
    pub frame_duration_us: u32,
    /// Total packet duration in microseconds.
    pub total_duration_us: u32,
    /// Number of channels (1 or 2).
    pub channels: u32,
    /// Number of PCM samples per channel at 48kHz.
    pub samples_per_channel: u32,
}

/// Parse the Opus TOC byte and extract packet information.
///
/// `data` is the raw Opus payload (NOT including OSTP/RTP headers).
/// Returns `None` if the packet is too small or malformed.
pub fn parse_packet_info(data: &[u8]) -> Option<OpusPacketInfo> {
    if data.is_empty() {
        return None;
    }

    let toc_byte = data[0];
    let config = (toc_byte >> 3) & 0x1F;
    let stereo = (toc_byte >> 2) & 1 != 0;
    let frame_count_code = toc_byte & 0x03;

    let toc = OpusToc {
        config,
        stereo,
        frame_count_code,
    };

    let frame_count = match frame_count_code {
        0 => 1,
        1 | 2 => 2,
        3 => {
            // Code 3: byte[1] contains the frame count
            if data.len() < 2 {
                return None;
            }
            (data[1] & 0x3F) as u32
        }
        _ => unreachable!(),
    };

    let frame_duration_us = FRAME_DURATIONS_US[config as usize];
    let total_duration_us = frame_duration_us * frame_count;
    let channels = if stereo { 2 } else { 1 };
    // At 48kHz: samples = duration_us * 48000 / 1_000_000 = duration_us * 48 / 1000
    let samples_per_channel = (total_duration_us as u64 * 48) / 1000;

    Some(OpusPacketInfo {
        toc,
        frame_count,
        frame_duration_us,
        total_duration_us,
        channels,
        samples_per_channel: samples_per_channel as u32,
    })
}

/// Generate silence (zeros) for an Opus frame of the given duration.
///
/// Returns the number of stereo samples (frames) written.
/// `out` is cleared and filled with zeros.
pub fn generate_silence(duration_us: u32, channels: u32, sample_rate: u32, out: &mut Vec<f32>) -> usize {
    let total_samples = (duration_us as u64 * sample_rate as u64 / 1_000_000) as usize;
    let sample_count = total_samples * channels as usize;
    out.clear();
    out.resize(sample_count, 0.0);
    total_samples
}

/// Convert f32 interleaved PCM to Opus-compatible S16LE for encoding.
///
/// This is used by the TX path: WASM f32 PCM → S16LE bytes → JS WebCodecs AudioEncoder.
pub fn f32_to_s16le(pcm: &[f32], out: &mut Vec<u8>) {
    out.clear();
    out.reserve(pcm.len() * 2);
    for &s in pcm {
        let clamped = s.clamp(-1.0, 1.0);
        let val = (clamped * 32767.0) as i16;
        out.extend_from_slice(&val.to_le_bytes());
    }
}

/// Opus encoder/decoder configuration constants.
pub mod config {
    /// Default Opus bitrate in bits per second.
    pub const DEFAULT_BITRATE: u32 = 128_000;
    /// Opus sample rate (always 48kHz).
    pub const SAMPLE_RATE: u32 = 48_000;
    /// Default frame size in samples at 48kHz (20ms).
    pub const DEFAULT_FRAME_SIZE: u32 = 960;
    /// Maximum Opus packet size in bytes.
    pub const MAX_PACKET_SIZE: usize = 4000;
    /// Opus payload type in OSTP.
    pub const PAYLOAD_TYPE: u8 = 98;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_single_frame_mono() {
        // TOC: config=0 (SILK NB 10ms), mono, code=0 (1 frame)
        // config=0 → 0b00000, stereo=0, code=0b00 → TOC byte = 0b00000_0_00 = 0x00
        let data = [0x00, 0xFF, 0xFF]; // TOC + dummy payload
        let info = parse_packet_info(&data).unwrap();
        assert_eq!(info.frame_count, 1);
        assert_eq!(info.frame_duration_us, 10000);
        assert_eq!(info.total_duration_us, 10000);
        assert_eq!(info.channels, 1);
        assert_eq!(info.samples_per_channel, 480); // 10ms at 48kHz
    }

    #[test]
    fn test_parse_stereo_20ms() {
        // TOC: config=1 (SILK NB 20ms), stereo=1, code=0 (1 frame)
        // config=1 → 0b00001, stereo=1, code=0b00 → TOC byte = 0b00001_1_00 = 0x0C
        let data = [0x0C, 0xFF];
        let info = parse_packet_info(&data).unwrap();
        assert_eq!(info.frame_count, 1);
        assert_eq!(info.frame_duration_us, 20000);
        assert_eq!(info.channels, 2);
        assert_eq!(info.samples_per_channel, 960); // 20ms at 48kHz
    }

    #[test]
    fn test_parse_celt_wb_10ms() {
        // config=26 (CELT WB 10ms) → 0b11010, stereo=0, code=0
        // TOC byte = 0b11010_0_00 = 0xD0
        let data = [0xD0, 0xFF];
        let info = parse_packet_info(&data).unwrap();
        assert_eq!(info.frame_count, 1);
        assert_eq!(info.frame_duration_us, 10000);
        assert_eq!(info.channels, 1);
        assert_eq!(info.samples_per_channel, 480);
    }

    #[test]
    fn test_parse_code3_multiple_frames() {
        // config=1 (20ms), mono, code=3 → TOC = 0b00001_0_11 = 0x0B
        // Byte[1] = frame count = 5 (0x05)
        let data = [0x0B, 0x05, 0xFF, 0xFF];
        let info = parse_packet_info(&data).unwrap();
        assert_eq!(info.frame_count, 5);
        assert_eq!(info.total_duration_us, 100000); // 5 * 20ms
        assert_eq!(info.samples_per_channel, 4800); // 100ms at 48kHz
    }

    #[test]
    fn test_empty_packet() {
        assert!(parse_packet_info(&[]).is_none());
    }

    #[test]
    fn test_generate_silence() {
        let mut out = Vec::new();
        let frames = generate_silence(20000, 2, 48000, &mut out);
        assert_eq!(frames, 960); // 20ms at 48kHz
        assert_eq!(out.len(), 1920); // 960 * 2 channels
        assert!(out.iter().all(|&s| s == 0.0));
    }

    #[test]
    fn test_f32_to_s16le() {
        let pcm = [0.0f32, 0.5, -0.5, 1.0, -1.0];
        let mut out = Vec::new();
        f32_to_s16le(&pcm, &mut out);
        assert_eq!(out.len(), 10); // 5 samples * 2 bytes

        // Check zero
        let s0 = i16::from_le_bytes([out[0], out[1]]);
        assert_eq!(s0, 0);

        // Check 0.5 → ~16383
        let s1 = i16::from_le_bytes([out[2], out[3]]);
        assert!((s1 - 16383).abs() <= 1);

        // Check -1.0 → -32767
        let s4 = i16::from_le_bytes([out[8], out[9]]);
        assert_eq!(s4, -32767);
    }
}
