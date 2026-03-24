//! Optional audio playback using cpal.
//! Enable with the `audio` feature.

use crate::adpcm::decode_adpcm_payload;
use crate::parser::{constants, OSTPacket};

/// Decode a packet's payload to interleaved float32 PCM samples.
pub fn decode_packet_to_f32(packet: &OSTPacket) -> Option<Vec<f32>> {
    if packet.payload_type == constants::PT_ADPCM_STEREO
        || packet.payload_type == constants::PT_ADPCM_MONO
    {
        decode_adpcm_to_f32(&packet.payload)
    } else if packet.payload_type == constants::PT_OPUS {
        decode_opus_to_f32(&packet.payload, packet.channels)
    } else if packet.payload_type == constants::PT_LC3 {
        decode_lc3_to_f32(&packet.payload, packet.channels)
    } else {
        decode_int32le_to_f32(&packet.payload)
    }
}

/// Decode int32 LE interleaved payload to float32.
fn decode_int32le_to_f32(payload: &[u8]) -> Option<Vec<f32>> {
    let total_samples = payload.len() / 4;
    if total_samples == 0 {
        return None;
    }

    let scale = 1.0 / i32::MAX as f32;
    let mut out = Vec::with_capacity(total_samples);

    for i in 0..total_samples {
        let offset = i * 4;
        if offset + 3 >= payload.len() {
            break;
        }
        let val = i32::from_le_bytes([
            payload[offset],
            payload[offset + 1],
            payload[offset + 2],
            payload[offset + 3],
        ]);
        out.push(val as f32 * scale);
    }

    Some(out)
}

/// Decode Opus payload to float32.
///
/// Requires the `opus` feature (audiopus crate).
/// Returns None if the feature is not enabled or decode fails.
#[cfg(feature = "opus")]
fn decode_opus_to_f32(payload: &[u8], channels: usize) -> Option<Vec<f32>> {
    use audiopus::{coder::Decoder, Channels, SampleRate};

    let ch = match channels {
        1 => Channels::Mono,
        _ => Channels::Stereo,
    };

    let mut decoder = Decoder::new(SampleRate::Hz48000, ch).ok()?;
    // 960 frames = 20ms at 48kHz
    let max_samples = 960 * channels;
    let mut output = vec![0.0f32; max_samples];

    match decoder.decode_float(Some(payload), &mut output, false) {
        Ok(decoded_samples) => {
            output.truncate(decoded_samples * channels);
            Some(output)
        }
        Err(e) => {
            eprintln!("[SolunaSDK] Opus decode error: {e}");
            None
        }
    }
}

#[cfg(not(feature = "opus"))]
fn decode_opus_to_f32(_payload: &[u8], _channels: usize) -> Option<Vec<f32>> {
    eprintln!("[SolunaSDK] Opus codec requires the 'opus' feature — add audiopus to Cargo.toml");
    None
}

/// Decode LC3 payload to float32.
///
/// LC3 decode requires liblc3 (Google, Apache 2.0).
/// https://github.com/google/liblc3
/// TODO: Add liblc3-sys bindings.
fn decode_lc3_to_f32(_payload: &[u8], _channels: usize) -> Option<Vec<f32>> {
    eprintln!("[SolunaSDK] LC3 codec not yet supported — install liblc3");
    None
}

/// Decode ADPCM payload to float32.
fn decode_adpcm_to_f32(payload: &[u8]) -> Option<Vec<f32>> {
    let pcm16 = decode_adpcm_payload(payload)?;
    Some(pcm16.iter().map(|&s| s as f32 / 32768.0).collect())
}

/// Convert interleaved samples to stereo (mono -> duplicate, multi-ch -> take first 2).
pub fn to_stereo(samples: &[f32], channels: usize) -> Vec<f32> {
    if channels == 2 {
        return samples.to_vec();
    }

    let frames = samples.len() / channels.max(1);
    let mut out = Vec::with_capacity(frames * 2);

    if channels == 1 {
        for &s in samples {
            out.push(s);
            out.push(s);
        }
    } else {
        for f in 0..frames {
            let base = f * channels;
            out.push(samples.get(base).copied().unwrap_or(0.0));
            out.push(samples.get(base + 1).copied().unwrap_or(0.0));
        }
    }

    out
}

#[cfg(feature = "audio")]
pub mod cpal_player {
    //! cpal-based audio output at 48kHz stereo float32.

    use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
    use std::sync::{Arc, Mutex};
    use std::collections::VecDeque;

    const SAMPLE_RATE: u32 = 48000;

    pub struct AudioPlayer {
        _stream: Option<cpal::Stream>,
        buffer: Arc<Mutex<VecDeque<f32>>>,
    }

    impl AudioPlayer {
        pub fn new() -> Self {
            Self {
                _stream: None,
                buffer: Arc::new(Mutex::new(VecDeque::new())),
            }
        }

        pub fn start(&mut self) -> Result<(), Box<dyn std::error::Error>> {
            let host = cpal::default_host();
            let device = host
                .default_output_device()
                .ok_or("No output device")?;

            let config = cpal::StreamConfig {
                channels: 2,
                sample_rate: cpal::SampleRate(SAMPLE_RATE),
                buffer_size: cpal::BufferSize::Default,
            };

            let buf = self.buffer.clone();
            let stream = device.build_output_stream(
                &config,
                move |data: &mut [f32], _info| {
                    let mut buf = buf.lock().unwrap();
                    for sample in data.iter_mut() {
                        *sample = buf.pop_front().unwrap_or(0.0);
                    }
                },
                |err| eprintln!("[SolunaSDK] cpal error: {err}"),
                None,
            )?;

            stream.play()?;
            self._stream = Some(stream);
            Ok(())
        }

        pub fn stop(&mut self) {
            self._stream = None;
        }

        /// Push interleaved stereo float32 samples.
        pub fn push_samples(&self, samples: &[f32]) {
            if let Ok(mut buf) = self.buffer.lock() {
                buf.extend(samples);
                // Cap buffer at ~500ms to prevent memory growth
                const MAX_SAMPLES: usize = 48000 * 2; // 500ms stereo
                while buf.len() > MAX_SAMPLES {
                    buf.pop_front();
                }
            }
        }
    }
}
