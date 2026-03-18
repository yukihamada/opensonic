use wasm_bindgen::prelude::*;
use soluna_core::{ostp, convert, downmix, opus};

/// Ring buffer capacity: ~2.7s at 48kHz stereo
const RING_FRAMES: usize = 131072;
/// Sample rate
const SAMPLE_RATE: u32 = 48000;
/// Per-source ring buffer capacity: 1s at 48kHz (talk mode)
const SOURCE_RING_FRAMES: usize = 48000;
/// Maximum simultaneous sources in talk mode
const MAX_SOURCES: usize = 64;
/// Source timeout in milliseconds (5 seconds of no packets → deactivate)
const SOURCE_TIMEOUT_MS: f64 = 5000.0;
/// Only mix the loudest N speakers at a time
const TOP_N: usize = 8;
/// RMS threshold for voice activity detection (float scale, ~0.002 is quiet room)
const VAD_THRESHOLD: f32 = 0.005;
/// AGC target RMS level (float scale)
const AGC_TARGET: f32 = 0.25;

// ── Per-source buffer for talk mode (SSRC-based demux) ────────────

struct SourceBuffer {
    ssrc: u32,
    ring_l: Vec<f32>,
    ring_r: Vec<f32>,
    write_pos: usize,
    read_pos: usize,
    last_seen_ms: f64,
    active: bool,
    tx_channels: u32,
    // VAD
    rms_energy: f32,
    voice_active: bool,
    // AGC
    agc_gain: f32,
    // Spatial panning
    pan_gain_l: f32,
    pan_gain_r: f32,
    // Smooth fade (0=silent, 1=full)
    fade: f32,
    // Slot index for pan assignment
    slot_index: usize,
}

impl SourceBuffer {
    fn new(cap: usize, slot: usize) -> Self {
        let (pan_l, pan_r) = Self::compute_pan(slot);
        Self {
            ssrc: 0,
            ring_l: vec![0.0; cap],
            ring_r: vec![0.0; cap],
            write_pos: 0,
            read_pos: 0,
            last_seen_ms: 0.0,
            active: false,
            tx_channels: 2,
            rms_energy: 0.0,
            voice_active: false,
            agc_gain: 1.0,
            pan_gain_l: pan_l,
            pan_gain_r: pan_r,
            fade: 0.0,
            slot_index: slot,
        }
    }

    // Equal-power panning: center, left30%, right30%, left60%, right60%, ...
    fn compute_pan(slot: usize) -> (f32, f32) {
        let pan = if slot == 0 {
            0.0f32
        } else {
            let spread = (0.3 * ((slot + 1) / 2) as f32).min(0.9);
            if slot % 2 == 1 { -spread } else { spread }
        };
        let angle = (pan + 1.0) * 0.5; // 0..1
        let l = (angle * std::f32::consts::FRAC_PI_2).cos();
        let r = (angle * std::f32::consts::FRAC_PI_2).sin();
        (l, r)
    }

    fn available(&self) -> usize {
        let cap = self.ring_l.len();
        if self.write_pos >= self.read_pos {
            self.write_pos - self.read_pos
        } else {
            cap - self.read_pos + self.write_pos
        }
    }

    /// Write interleaved PCM from conv_buf into this source's L/R ring.
    fn write_pcm(&mut self, conv_buf: &[f32], tx_ch: usize) {
        let frame_count = if tx_ch > 0 { conv_buf.len() / tx_ch } else { 0 };
        if frame_count == 0 {
            return;
        }

        // Update RMS energy for VAD
        let sum_sq: f64 = conv_buf.iter().map(|&s| (s as f64) * (s as f64)).sum();
        let frame_rms = (sum_sq / conv_buf.len().max(1) as f64).sqrt() as f32;
        self.rms_energy = self.rms_energy * 0.85 + frame_rms * 0.15;
        self.voice_active = self.rms_energy > VAD_THRESHOLD;

        // AGC: adjust gain toward target
        if self.voice_active && frame_rms > 0.0 {
            let desired = (AGC_TARGET / frame_rms).clamp(0.1, 4.0);
            let alpha = if desired < self.agc_gain { 0.01 } else { 0.001 };
            self.agc_gain = self.agc_gain * (1.0 - alpha) + desired * alpha;
        }

        let cap = self.ring_l.len();
        for f in 0..frame_count {
            let src_off = f * tx_ch;
            let src_frame = &conv_buf[src_off..src_off + tx_ch];
            let w = self.write_pos % cap;

            if tx_ch == 1 {
                self.ring_l[w] = src_frame[0];
                self.ring_r[w] = src_frame[0];
            } else if tx_ch == 2 {
                self.ring_l[w] = src_frame[0];
                self.ring_r[w] = src_frame[1];
            } else {
                let mut stereo = [0.0f32; 2];
                downmix::downmix_frame(src_frame, tx_ch, &mut stereo, 2);
                self.ring_l[w] = stereo[0];
                self.ring_r[w] = stereo[1];
            }

            self.write_pos += 1;
        }
    }
}

// ── JS callbacks for Opus encode/decode via WebCodecs ──────────────

#[wasm_bindgen]
extern "C" {
    /// Called when WASM receives a PT_OPUS packet that needs decoding.
    /// JS should decode with WebCodecs AudioDecoder and call push_decoded_opus().
    /// Parameters: opus_data (raw Opus bytes), sequence number, timestamp, channels.
    #[wasm_bindgen(js_namespace = ["globalThis", "SolunaOpusBridge"])]
    fn on_opus_packet(data: &[u8], seq: u16, timestamp: u32, channels: u32);

    /// Called when SolunaTx wants to encode PCM as Opus.
    /// JS should encode with WebCodecs AudioEncoder and return compressed bytes.
    /// Parameters: f32 PCM interleaved, channels, sample_rate.
    /// Returns: nothing (async — JS calls tx_push_encoded_opus when done).
    #[wasm_bindgen(js_namespace = ["globalThis", "SolunaOpusBridge"])]
    fn on_opus_encode(pcm: &[f32], channels: u32, sample_rate: u32, seq: u16, timestamp: u32, ssrc: u32, stream_id: u16);
}

/// Check if the JS Opus bridge is available.
fn has_opus_bridge() -> bool {
    // In wasm, we check if the global SolunaOpusBridge object exists
    // by trying to call a probe function. If it throws, the bridge isn't set up.
    // We use a simpler approach: a static flag set from JS.
    OPUS_BRIDGE_AVAILABLE.load(core::sync::atomic::Ordering::Relaxed)
}

static OPUS_BRIDGE_AVAILABLE: core::sync::atomic::AtomicBool =
    core::sync::atomic::AtomicBool::new(false);

/// JS calls this to signal that SolunaOpusBridge is available.
#[wasm_bindgen]
pub fn set_opus_bridge_available(available: bool) {
    OPUS_BRIDGE_AVAILABLE.store(available, core::sync::atomic::Ordering::Relaxed);
}

#[wasm_bindgen]
pub struct SolunaPlayer {
    /// Interleaved f32 ring buffer (stereo output)
    ring_l: Vec<f32>,
    ring_r: Vec<f32>,
    cap: usize,
    write_pos: usize,
    read_pos: usize,
    /// Output channel count (1=mono, 2=stereo)
    out_channels: u32,
    /// Detected TX channel count from stream_id
    tx_channels: u32,
    /// Packets received
    pkt_count: u32,
    /// Underrun count
    underruns: u32,
    /// Opus packets received (pending JS decode)
    opus_pkt_count: u32,
    /// Opus packets decoded (fed back from JS)
    opus_decoded_count: u32,
    /// Temp buffer for format conversion
    conv_buf: Vec<f32>,

    // ── Sync state ──────────────────────────────────
    /// Sync mode enabled
    sync_enabled: bool,
    /// Target playout delay in ms (default 80ms, matching iOS)
    sync_delay_ms: u32,
    /// Target fill level in frames (computed from sync_delay_ms)
    target_fill_frames: u32,
    /// Last received media_timestamp from OSTP header
    last_media_ts: u32,
    /// Sync convergence counter (fast α initially, slow when stable)
    sync_samples_count: u32,
    /// Net delay history for median filter (5 samples)
    sync_history: [i32; 5],
    sync_history_idx: u32,
    /// Current estimated net delay in ns
    net_delay_ns: i32,
    /// Clock offset from relay in nanoseconds (relay_time - local_time).
    /// Set by JS via set_clock_offset() after NTP-like exchange over WebSocket.
    clock_offset_ns: i64,

    // ── Talk mode: multi-source mixing ───────────────
    /// Per-SSRC ring buffers for simultaneous sources
    sources: Vec<SourceBuffer>,
    /// When true, demux by SSRC and mix all active sources
    talk_mode: bool,
}

#[wasm_bindgen]
impl SolunaPlayer {
    /// Create a new player. `out_channels` = 1 (mono) or 2 (stereo).
    #[wasm_bindgen(constructor)]
    pub fn new(out_channels: u32) -> Self {
        let sync_delay_ms = 80u32;
        let target_fill = sync_delay_ms * SAMPLE_RATE / 1000; // 3840 frames at 80ms
        Self {
            ring_l: vec![0.0; RING_FRAMES],
            ring_r: vec![0.0; RING_FRAMES],
            cap: RING_FRAMES,
            write_pos: 0,
            read_pos: 0,
            out_channels: out_channels.max(1).min(2),
            tx_channels: 2,
            pkt_count: 0,
            underruns: 0,
            opus_pkt_count: 0,
            opus_decoded_count: 0,
            conv_buf: Vec::with_capacity(2048),
            sync_enabled: true,
            sync_delay_ms,
            target_fill_frames: target_fill,
            last_media_ts: 0,
            sync_samples_count: 0,
            sync_history: [0i32; 5],
            sync_history_idx: 0,
            net_delay_ns: 0,
            clock_offset_ns: 0,
            sources: (0..MAX_SOURCES).map(|i| SourceBuffer::new(SOURCE_RING_FRAMES, i)).collect(),
            talk_mode: false,
        }
    }

    /// Push a raw OSTP packet (as received from WebSocket).
    /// Parses header, converts format, downmixes, and writes to ring buffer.
    /// For PT_OPUS: forwards to JS WebCodecs bridge for async decode.
    pub fn push_packet(&mut self, data: &[u8]) {
        self.push_packet_with_time(data, js_sys::Date::now())
    }

    /// Push packet with explicit wall-clock time (ms).
    pub fn push_packet_with_time(&mut self, data: &[u8], now_ms: f64) {
        let pkt = match ostp::parse(data) {
            Some(p) => p,
            None => {
                self.push_raw_s16(data);
                return;
            }
        };

        self.tx_channels = pkt.tx_channels;
        self.pkt_count += 1;

        // ── Sync: update buffer target from media_timestamp ──
        if self.sync_enabled && pkt.ostp.media_timestamp != 0 {
            self.last_media_ts = pkt.ostp.media_timestamp;
            self.update_sync_target(pkt.ostp.media_timestamp, now_ms);
        }

        let ssrc = pkt.rtp.ssrc;

        // ── PT_OPUS: delegate to JS WebCodecs bridge ──
        if pkt.rtp.payload_type == ostp::PT_OPUS {
            self.opus_pkt_count += 1;

            if has_opus_bridge() {
                // Forward raw Opus payload to JS for decoding via WebCodecs AudioDecoder.
                // JS will call push_decoded_opus() with the resulting f32 PCM.
                // NOTE: For talk mode, Opus decoded PCM comes back via push_decoded_opus_ssrc().
                on_opus_packet(
                    pkt.payload,
                    pkt.rtp.sequence,
                    pkt.rtp.timestamp,
                    pkt.tx_channels,
                );
            } else {
                // No bridge available — generate silence matching the Opus frame duration
                // so playback timing stays correct.
                if let Some(info) = opus::parse_packet_info(pkt.payload) {
                    let tx_ch = pkt.tx_channels.max(info.channels);
                    opus::generate_silence(
                        info.total_duration_us,
                        tx_ch,
                        SAMPLE_RATE,
                        &mut self.conv_buf,
                    );
                    if self.talk_mode {
                        self.write_pcm_to_source(ssrc, tx_ch as usize, now_ms);
                    } else {
                        self.write_pcm_to_ring(tx_ch as usize);
                    }
                }
            }
            return;
        }

        // Convert payload based on payload type
        let tx_ch = pkt.tx_channels as usize;
        match pkt.rtp.payload_type {
            ostp::PT_PCM24 | ostp::PT_AES67_L24 => {
                convert::s24le_to_f32(pkt.payload, &mut self.conv_buf);
            }
            ostp::PT_AES67_L16 => {
                convert::s16le_to_f32(pkt.payload, &mut self.conv_buf);
            }
            ostp::PT_F32 => {
                convert::f32le_to_f32(pkt.payload, &mut self.conv_buf);
            }
            _ => {
                convert::s32le_to_f32(pkt.payload, &mut self.conv_buf);
            }
        }

        if self.talk_mode {
            self.write_pcm_to_source(ssrc, tx_ch, now_ms);
        } else {
            self.write_pcm_to_ring(tx_ch);
        }
    }

    /// Push pre-decoded Opus PCM data (called from JS after WebCodecs decode).
    ///
    /// `pcm` is interleaved f32 samples, `channels` is the channel count.
    /// In talk mode, use `push_decoded_opus_ssrc()` instead for proper source demux.
    pub fn push_decoded_opus(&mut self, pcm: &[f32], channels: u32) {
        self.opus_decoded_count += 1;
        let ch = channels.max(1) as usize;
        self.conv_buf.clear();
        self.conv_buf.extend_from_slice(pcm);
        // In non-talk mode, write to the single shared ring buffer
        self.write_pcm_to_ring(ch);
    }

    /// Push pre-decoded Opus PCM with SSRC for talk-mode demux.
    ///
    /// JS should call this (instead of push_decoded_opus) when talk_mode is active,
    /// passing the SSRC from the original RTP packet.
    pub fn push_decoded_opus_ssrc(&mut self, pcm: &[f32], channels: u32, ssrc: u32) {
        self.opus_decoded_count += 1;
        let ch = channels.max(1) as usize;
        self.conv_buf.clear();
        self.conv_buf.extend_from_slice(pcm);
        if self.talk_mode {
            let now_ms = js_sys::Date::now();
            self.write_pcm_to_source(ssrc, ch, now_ms);
        } else {
            self.write_pcm_to_ring(ch);
        }
    }

    /// Write conv_buf contents to the L/R ring buffer with downmix.
    fn write_pcm_to_ring(&mut self, tx_ch: usize) {
        let frame_count = if tx_ch > 0 { self.conv_buf.len() / tx_ch } else { 0 };
        if frame_count == 0 {
            return;
        }

        for f in 0..frame_count {
            let src_off = f * tx_ch;
            let src_frame = &self.conv_buf[src_off..src_off + tx_ch];
            let w = self.write_pos % self.cap;

            if tx_ch == 1 {
                self.ring_l[w] = src_frame[0];
                self.ring_r[w] = src_frame[0];
            } else if tx_ch == 2 {
                self.ring_l[w] = src_frame[0];
                self.ring_r[w] = src_frame[1];
            } else {
                let mut stereo = [0.0f32; 2];
                downmix::downmix_frame(src_frame, tx_ch, &mut stereo, 2);
                self.ring_l[w] = stereo[0];
                self.ring_r[w] = stereo[1];
            }

            self.write_pos += 1;
        }
    }

    /// Update sync target based on media_timestamp.
    /// Matches iOS AudioReceiverBridge.mm L1793-1840.
    fn update_sync_target(&mut self, media_ts: u32, now_ms: f64) {
        // Convert JS wall-clock (ms) to 32-bit nanosecond (same truncation as iOS CLOCK_REALTIME)
        let now_ns64 = (now_ms * 1_000_000.0) as u64;
        let now_ns32 = (now_ns64 & 0xFFFF_FFFF) as u32;

        // Apply clock offset correction: adjust local time to relay/TX time frame
        let offset_ns32 = (self.clock_offset_ns & 0xFFFF_FFFF) as i32;
        let net_delay_ns = (now_ns32.wrapping_sub(media_ts) as i32) + offset_ns32;

        // Reject absurd values (negative or > 2 seconds)
        if net_delay_ns < 0 || net_delay_ns > 2_000_000_000 {
            return;
        }

        self.net_delay_ns = net_delay_ns;

        // Median filter: store in history
        let idx = (self.sync_history_idx % 5) as usize;
        self.sync_history[idx] = net_delay_ns;
        self.sync_history_idx += 1;

        // Compute target buffer fill from sync delay
        let sync_delay_ns = (self.sync_delay_ms as i32) * 1_000_000;
        let buffer_ns = sync_delay_ns - net_delay_ns;
        let buffer_ns = buffer_ns.max(5_000_000); // 5ms floor

        let target = ((buffer_ns as i64 * SAMPLE_RATE as i64) / 1_000_000_000) as u32;

        // Adaptive EMA (matches iOS)
        let prev = self.target_fill_frames;
        let diff = (target as i32) - (prev as i32);
        let alpha = if self.sync_samples_count < 50 {
            self.sync_samples_count += 1;
            0.20 // First ~250ms: fast lock-on
        } else if diff.abs() > 2400 {
            0.15 // >50ms jump: re-converge
        } else if diff.abs() > 480 {
            0.08 // 10-50ms drift: moderate
        } else {
            0.02 // Stable: gentle smoothing
        };

        let smoothed = (prev as f64 * (1.0 - alpha) + target as f64 * alpha) as u32;
        self.target_fill_frames = smoothed.max(480); // 10ms minimum
    }

    /// Fallback: push raw S16LE PCM (no OSTP header).
    fn push_raw_s16(&mut self, data: &[u8]) {
        convert::s16le_to_f32(data, &mut self.conv_buf);
        let ch = self.tx_channels as usize;
        let frame_count = if ch > 0 { self.conv_buf.len() / ch } else { self.conv_buf.len() / 2 };
        let ch = if ch == 0 { 2 } else { ch };

        for f in 0..frame_count {
            let w = self.write_pos % self.cap;
            if ch == 1 {
                let s = self.conv_buf[f];
                self.ring_l[w] = s;
                self.ring_r[w] = s;
            } else {
                self.ring_l[w] = self.conv_buf[f * ch];
                self.ring_r[w] = self.conv_buf[f * ch + 1];
            }
            self.write_pos += 1;
        }
        self.pkt_count += 1;
    }

    /// Pull audio for AudioWorklet (128 frames per call).
    /// Sync mode: skips or pads samples to converge to target fill level.
    /// Talk mode: mixes all active SSRC sources together.
    pub fn pull_audio(&mut self, out_l: &mut [f32], out_r: &mut [f32]) -> u32 {
        let n = out_l.len().min(out_r.len());

        // ── Talk mode: smart mixing with VAD + Top-N + spatial panning + AGC ──
        if self.talk_mode {
            let now_ms = js_sys::Date::now();

            for i in 0..n {
                out_l[i] = 0.0;
                out_r[i] = 0.0;
            }

            // Step 1: Collect active sources with enough data, sorted by energy
            let mut candidates: Vec<(usize, f32)> = Vec::new(); // (index, energy)
            for (idx, src) in self.sources.iter_mut().enumerate() {
                if !src.active { continue; }
                if now_ms - src.last_seen_ms > SOURCE_TIMEOUT_MS {
                    src.active = false;
                    src.fade = 0.0;
                    continue;
                }
                if src.available() < n { continue; }
                candidates.push((idx, src.rms_energy));
            }
            // Sort by energy descending (loudest first)
            candidates.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(std::cmp::Ordering::Equal));

            // Step 2: Mix Top-N voice-active sources with spatial panning + AGC
            let mut mixed_count = 0usize;
            for (rank, &(src_idx, _)) in candidates.iter().enumerate() {
                let src = &mut self.sources[src_idx];
                let should_mix = rank < TOP_N && src.voice_active;
                let target_fade: f32 = if should_mix { 1.0 } else { 0.0 };

                let gain = src.agc_gain;
                let pan_l = src.pan_gain_l;
                let pan_r = src.pan_gain_r;
                let cap = src.ring_l.len();

                for f in 0..n {
                    // Smooth fade
                    if src.fade < target_fade {
                        src.fade = (src.fade + 0.02).min(1.0);
                    } else if src.fade > target_fade {
                        src.fade = (src.fade - 0.005).max(0.0);
                    }

                    if src.fade < 0.001 { continue; }

                    let frame_gain = gain * src.fade;
                    let idx = src.read_pos % cap;
                    let sl = src.ring_l[idx] * frame_gain;
                    let sr = src.ring_r[idx] * frame_gain;
                    src.read_pos += 1;

                    if self.out_channels == 1 {
                        out_l[f] += (sl + sr) * 0.5;
                    } else {
                        out_l[f] += sl * pan_l;
                        out_r[f] += sr * pan_r;
                    }
                }

                if src.fade > 0.001 { mixed_count += 1; }
            }

            if mixed_count == 0 {
                self.underruns += 1;
                return 0;
            }

            // Clamp output
            for i in 0..n {
                out_l[i] = out_l[i].clamp(-1.0, 1.0);
                out_r[i] = out_r[i].clamp(-1.0, 1.0);
            }
            return n as u32;
        }

        // ── Single-source mode (original behavior) ──
        let avail = self.write_pos.wrapping_sub(self.read_pos);

        if avail < n {
            for i in 0..n {
                out_l[i] = 0.0;
                out_r[i] = 0.0;
            }
            self.underruns += 1;
            return 0;
        }

        // ── Sync: adjust read position to converge to target fill ──
        if self.sync_enabled && self.sync_samples_count > 10 {
            let target = self.target_fill_frames as usize;
            if avail > target + 480 {
                // Too much buffered → skip 1 sample to catch up (gentle)
                self.read_pos += 1;
            } else if avail < target.saturating_sub(480) && avail > n + 1 {
                // Too little buffered → re-read 1 sample to slow down
                if self.read_pos > 0 {
                    self.read_pos -= 1;
                }
            }
        }

        if self.out_channels == 1 {
            for i in 0..n {
                let r = (self.read_pos + i) % self.cap;
                let mono = (self.ring_l[r] + self.ring_r[r]) * 0.5;
                out_l[i] = mono;
                out_r[i] = mono;
            }
        } else {
            for i in 0..n {
                let r = (self.read_pos + i) % self.cap;
                out_l[i] = self.ring_l[r];
                out_r[i] = self.ring_r[r];
            }
        }

        self.read_pos += n;
        n as u32
    }

    /// Number of frames available for reading.
    pub fn available(&self) -> u32 {
        self.write_pos.wrapping_sub(self.read_pos) as u32
    }

    /// Total packets received.
    pub fn packet_count(&self) -> u32 {
        self.pkt_count
    }

    /// Total underruns.
    pub fn underrun_count(&self) -> u32 {
        self.underruns
    }

    /// Detected TX channel count.
    pub fn detected_tx_channels(&self) -> u32 {
        self.tx_channels
    }

    /// Opus packets received (forwarded to JS for decode).
    pub fn opus_packet_count(&self) -> u32 {
        self.opus_pkt_count
    }

    /// Opus packets successfully decoded (fed back from JS).
    pub fn opus_decoded_count(&self) -> u32 {
        self.opus_decoded_count
    }

    /// Set output channel mode (1=mono, 2=stereo).
    pub fn set_output_channels(&mut self, ch: u32) {
        self.out_channels = ch.max(1).min(2);
    }

    /// Enable/disable sync mode.
    pub fn set_sync_enabled(&mut self, enabled: bool) {
        self.sync_enabled = enabled;
        if !enabled {
            self.sync_samples_count = 0;
        }
    }

    /// Set sync playout delay in milliseconds (default 80).
    /// Higher = more latency but more stable. Lower = tighter sync but more risk of underrun.
    pub fn set_sync_delay_ms(&mut self, ms: u32) {
        self.sync_delay_ms = ms.max(20).min(500);
        self.target_fill_frames = self.sync_delay_ms * SAMPLE_RATE / 1000;
    }

    /// Get current sync delay in ms.
    pub fn sync_delay_ms(&self) -> u32 {
        self.sync_delay_ms
    }

    /// Get current target fill level in frames.
    pub fn target_fill_frames(&self) -> u32 {
        self.target_fill_frames
    }

    /// Get estimated network delay in ms.
    pub fn net_delay_ms(&self) -> f32 {
        self.net_delay_ns as f32 / 1_000_000.0
    }

    /// Is sync locked? (converged after initial samples)
    pub fn sync_locked(&self) -> bool {
        self.sync_samples_count >= 50
    }

    /// Set clock offset in milliseconds (from JS NTP-like measurement).
    /// Positive = local clock is behind relay, negative = ahead.
    pub fn set_clock_offset(&mut self, offset_ms: f64) {
        self.clock_offset_ns = (offset_ms * 1_000_000.0) as i64;
    }

    /// Get current clock offset in milliseconds.
    pub fn clock_offset_ms(&self) -> f64 {
        self.clock_offset_ns as f64 / 1_000_000.0
    }

    /// Clear the ring buffer.
    pub fn clear(&mut self) {
        self.read_pos = self.write_pos;
        self.sync_samples_count = 0;
    }

    /// Enable or disable talk mode (multi-source mixing by SSRC).
    #[wasm_bindgen]
    pub fn set_talk_mode(&mut self, enabled: bool) {
        self.talk_mode = enabled;
        if !enabled {
            // Deactivate all source buffers when leaving talk mode
            for src in &mut self.sources {
                src.active = false;
            }
        }
    }

    /// Whether talk mode is enabled.
    pub fn talk_mode(&self) -> bool {
        self.talk_mode
    }

    /// Number of currently active sources in talk mode.
    pub fn active_source_count(&self) -> u32 {
        self.sources.iter().filter(|s| s.active).count() as u32
    }

    /// Find the SourceBuffer index for a given SSRC, or allocate a new slot.
    /// Returns None if all slots are full.
    fn find_or_alloc_source(&mut self, ssrc: u32, now_ms: f64) -> Option<usize> {
        // First pass: exact match
        for (i, src) in self.sources.iter().enumerate() {
            if src.active && src.ssrc == ssrc {
                return Some(i);
            }
        }
        // Second pass: find an inactive slot
        for (i, src) in self.sources.iter().enumerate() {
            if !src.active {
                let slot = self.sources[i].slot_index;
                let (pl, pr) = SourceBuffer::compute_pan(slot);
                self.sources[i].ssrc = ssrc;
                self.sources[i].active = true;
                self.sources[i].write_pos = 0;
                self.sources[i].read_pos = 0;
                self.sources[i].last_seen_ms = now_ms;
                self.sources[i].rms_energy = 0.0;
                self.sources[i].voice_active = false;
                self.sources[i].agc_gain = 1.0;
                self.sources[i].fade = 0.0;
                self.sources[i].pan_gain_l = pl;
                self.sources[i].pan_gain_r = pr;
                return Some(i);
            }
        }
        // Third pass: evict oldest non-speaking source
        let mut oldest_idx = 0;
        let mut oldest_ms = f64::MAX;
        for (i, src) in self.sources.iter().enumerate() {
            if !src.voice_active && src.last_seen_ms < oldest_ms {
                oldest_ms = src.last_seen_ms;
                oldest_idx = i;
            }
        }
        let slot = self.sources[oldest_idx].slot_index;
        let (pl, pr) = SourceBuffer::compute_pan(slot);
        self.sources[oldest_idx].ssrc = ssrc;
        self.sources[oldest_idx].active = true;
        self.sources[oldest_idx].write_pos = 0;
        self.sources[oldest_idx].read_pos = 0;
        self.sources[oldest_idx].last_seen_ms = now_ms;
        self.sources[oldest_idx].rms_energy = 0.0;
        self.sources[oldest_idx].voice_active = false;
        self.sources[oldest_idx].agc_gain = 1.0;
        self.sources[oldest_idx].fade = 0.0;
        self.sources[oldest_idx].pan_gain_l = pl;
        self.sources[oldest_idx].pan_gain_r = pr;
        Some(oldest_idx)
    }

    /// Write conv_buf contents to the appropriate SourceBuffer (talk mode).
    fn write_pcm_to_source(&mut self, ssrc: u32, tx_ch: usize, now_ms: f64) {
        if let Some(idx) = self.find_or_alloc_source(ssrc, now_ms) {
            self.sources[idx].last_seen_ms = now_ms;
            self.sources[idx].tx_channels = tx_ch as u32;
            // We need to clone conv_buf because write_pcm borrows it
            let buf: Vec<f32> = self.conv_buf.clone();
            self.sources[idx].write_pcm(&buf, tx_ch);
        }
    }
}

/// TX encoder for browser microphone → OSTP packets.
/// Supports both raw PCM (PT_F32=97) and Opus (PT_OPUS=98) encoding.
#[wasm_bindgen]
pub struct SolunaTx {
    channels: u32,
    stream_id: u16,
    sequence: u16,
    timestamp: u32,
    ssrc: u32,
    pkt_buf: Vec<u8>,
    /// When true, TX uses Opus encoding via JS WebCodecs bridge.
    opus_enabled: bool,
}

#[wasm_bindgen]
impl SolunaTx {
    #[wasm_bindgen(constructor)]
    pub fn new(channels: u32, stream_id_base: u16) -> Self {
        let ch = channels.max(1).min(8);
        let stream_id = ((ch as u16) << 12) | (stream_id_base & 0x0FFF);
        let ssrc = (js_sys::Date::now() as u32) ^ 0xDEAD_BEEF;
        Self {
            channels: ch,
            stream_id,
            sequence: 0,
            timestamp: 0,
            ssrc,
            pkt_buf: vec![0u8; 24 + 12288 + 4],
            opus_enabled: false,
        }
    }

    /// Enable or disable Opus encoding for TX.
    /// When enabled, encode_frame() delegates to JS WebCodecs AudioEncoder.
    /// The JS bridge calls build_opus_packet() with the compressed bytes.
    pub fn set_opus_enabled(&mut self, enabled: bool) {
        self.opus_enabled = enabled;
    }

    /// Whether Opus encoding is enabled.
    pub fn opus_enabled(&self) -> bool {
        self.opus_enabled
    }

    /// Encode f32 PCM into an OSTP packet.
    ///
    /// If Opus is enabled AND the JS bridge is available, this forwards PCM
    /// to JS for Opus encoding. The JS bridge calls `build_opus_packet()` with
    /// the compressed bytes. In this case, returns an empty Vec (packet is sent async).
    ///
    /// If Opus is disabled or bridge unavailable, returns a raw PCM OSTP packet (PT_F32).
    pub fn encode_frame(&mut self, pcm: &[f32]) -> Vec<u8> {
        let frame_count = pcm.len() / self.channels as usize;

        if self.opus_enabled && has_opus_bridge() {
            // Delegate to JS WebCodecs for Opus encoding.
            // JS will call build_opus_packet() with compressed bytes.
            on_opus_encode(
                pcm,
                self.channels,
                SAMPLE_RATE,
                self.sequence,
                self.timestamp,
                self.ssrc,
                self.stream_id,
            );
            self.sequence = self.sequence.wrapping_add(1);
            self.timestamp = self.timestamp.wrapping_add(frame_count as u32);
            return Vec::new(); // Packet is built async in JS → build_opus_packet()
        }

        // Fallback: raw PCM encoding (original behavior)
        self.encode_frame_pcm(pcm)
    }

    /// Build an OSTP packet from pre-encoded Opus data.
    ///
    /// Called from JS after WebCodecs AudioEncoder produces compressed bytes.
    /// Returns the complete OSTP packet ready to send over WebSocket.
    pub fn build_opus_packet(
        &mut self,
        opus_data: &[u8],
        seq: u16,
        timestamp: u32,
    ) -> Vec<u8> {
        let payload_size = opus_data.len();
        let total_size = 24 + payload_size + 4; // header + payload + CRC

        let mut pkt = vec![0u8; total_size];

        // RTP header (12 bytes)
        pkt[0] = 0x90; // V=2, P=0, X=1, CC=0
        pkt[1] = ostp::PT_OPUS; // PT=98
        pkt[2..4].copy_from_slice(&seq.to_be_bytes());
        pkt[4..8].copy_from_slice(&timestamp.to_be_bytes());
        pkt[8..12].copy_from_slice(&self.ssrc.to_be_bytes());

        // RTP Extension header (4 bytes)
        pkt[12..14].copy_from_slice(&0x4F53u16.to_be_bytes()); // "OS"
        pkt[14..16].copy_from_slice(&2u16.to_be_bytes());

        // OSTP header (8 bytes)
        pkt[16..18].copy_from_slice(&self.stream_id.to_be_bytes());
        pkt[18..20].copy_from_slice(&0u16.to_be_bytes());
        let media_ts = (js_sys::Date::now() * 1_000_000.0) as u32;
        pkt[20..24].copy_from_slice(&media_ts.to_be_bytes());

        // Opus payload
        let payload_off = 24;
        pkt[payload_off..payload_off + payload_size].copy_from_slice(opus_data);

        // CRC-32 trailer
        let crc_off = payload_off + payload_size;
        let crc = crc32_ieee(&pkt[payload_off..crc_off]);
        pkt[crc_off..crc_off + 4].copy_from_slice(&crc.to_be_bytes());

        pkt
    }

    /// Encode f32 PCM into OSTP packet with raw PCM payload (PT_F32=97).
    fn encode_frame_pcm(&mut self, pcm: &[f32]) -> Vec<u8> {
        let frame_count = pcm.len() / self.channels as usize;
        let payload_size = pcm.len() * 4;
        let total_size = 24 + payload_size + 4;

        if total_size > self.pkt_buf.len() {
            self.pkt_buf.resize(total_size, 0);
        }

        // RTP header (12 bytes)
        self.pkt_buf[0] = 0x90; // V=2, P=0, X=1, CC=0
        self.pkt_buf[1] = 96;   // PT=96
        self.pkt_buf[2..4].copy_from_slice(&self.sequence.to_be_bytes());
        self.pkt_buf[4..8].copy_from_slice(&self.timestamp.to_be_bytes());
        self.pkt_buf[8..12].copy_from_slice(&self.ssrc.to_be_bytes());

        // RTP Extension header (4 bytes)
        self.pkt_buf[12..14].copy_from_slice(&0x4F53u16.to_be_bytes());
        self.pkt_buf[14..16].copy_from_slice(&2u16.to_be_bytes());

        // OSTP header (8 bytes) — media_timestamp = wall-clock nanoseconds (32-bit truncated)
        self.pkt_buf[16..18].copy_from_slice(&self.stream_id.to_be_bytes());
        self.pkt_buf[18..20].copy_from_slice(&0u16.to_be_bytes());
        let media_ts = (js_sys::Date::now() * 1_000_000.0) as u32;
        self.pkt_buf[20..24].copy_from_slice(&media_ts.to_be_bytes());

        // Payload: f32 → S32LE
        let payload_off = 24;
        for (i, &s) in pcm.iter().enumerate() {
            let val = (s * 2_147_483_647.0) as i32;
            let off = payload_off + i * 4;
            self.pkt_buf[off..off + 4].copy_from_slice(&val.to_le_bytes());
        }

        // CRC-32 trailer (big-endian, over payload only — matches C++)
        let crc_off = payload_off + payload_size;
        let crc = crc32_ieee(&self.pkt_buf[payload_off..crc_off]);
        self.pkt_buf[crc_off..crc_off + 4].copy_from_slice(&crc.to_be_bytes());

        self.sequence = self.sequence.wrapping_add(1);
        self.timestamp = self.timestamp.wrapping_add(frame_count as u32);

        self.pkt_buf[..total_size].to_vec()
    }
}

fn crc32_ieee(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xEDB8_8320 & (0u32.wrapping_sub(crc & 1)));
        }
    }
    crc ^ 0xFFFF_FFFF
}
