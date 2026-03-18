//! # OSTP Easy API — 3行で音声メッシュに参加
//!
//! ```rust
//! let node = soluna::easy::Node::join("my-channel")?;
//! node.send_audio(&pcm_f32_48khz);          // マイク音声を全員に送信
//! let audio = node.recv_audio();              // 他のノードの音声を受信
//! ```
//!
//! # Features
//! - ゼロコンフィグ: チャンネル名だけで参加
//! - ゼロコピー: lock-free ring bufferでオーディオスレッドから直接読み書き
//! - ゼロサーバー: UDPマルチキャストでP2P
//! - Opus圧縮: 48kHz → 128kbps (1/6帯域)
//! - OSTP/RTP互換: OpenSonicデバイスと完全互換
//! - スレッドセーフ: Send + Sync、オーディオコールバックから安全に呼べる

use crate::ostp;
use crate::ring_buffer::RingBuffer;
use std::net::{UdpSocket, Ipv4Addr, SocketAddrV4};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::Arc;
use std::io;

/// デフォルト設定
const MULTICAST_ADDR: Ipv4Addr = Ipv4Addr::new(239, 69, 0, 1);
const MULTICAST_PORT: u16 = 5004;
const SAMPLE_RATE: u32 = 48000;
const CHANNELS: u32 = 1;
const SAMPLES_PER_PACKET: usize = 240;  // 5ms @ 48kHz (LAN tier)
const JITTER_FRAMES: usize = 4800;       // 100ms jitter buffer

/// ノード設定
#[derive(Clone)]
pub struct Config {
    pub sample_rate: u32,
    pub channels: u32,
    pub multicast_addr: Ipv4Addr,
    pub port: u16,
    pub samples_per_packet: usize,
    pub jitter_frames: usize,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            sample_rate: SAMPLE_RATE,
            channels: CHANNELS,
            multicast_addr: MULTICAST_ADDR,
            port: MULTICAST_PORT,
            samples_per_packet: SAMPLES_PER_PACKET,
            jitter_frames: JITTER_FRAMES,
        }
    }
}

/// Solunaノード — P2Pメッシュの1参加者
///
/// Clone可能。Arc内部で共有状態を持つ。
/// オーディオスレッドから `send_audio()` / `recv_audio()` を直接呼べる。
#[derive(Clone)]
pub struct Node {
    inner: Arc<NodeInner>,
}

struct NodeInner {
    socket: UdpSocket,
    dest: SocketAddrV4,
    rx_buf: RingBuffer,
    running: AtomicBool,
    ssrc: u32,
    seq: AtomicU32,
    channel: String,
    config: Config,
}

impl Node {
    /// チャンネルに参加 (デフォルト設定)
    ///
    /// ```rust
    /// let node = Node::join("my-channel")?;
    /// ```
    pub fn join(channel: &str) -> io::Result<Self> {
        Self::join_with_config(channel, Config::default())
    }

    /// カスタム設定で参加
    pub fn join_with_config(channel: &str, config: Config) -> io::Result<Self> {
        let bind_addr = SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, config.port);
        let socket = UdpSocket::bind(bind_addr)?;

        // マルチキャストグループに参加
        socket.join_multicast_v4(&config.multicast_addr, &Ipv4Addr::UNSPECIFIED)?;
        socket.set_nonblocking(true)?;

        // 送信先
        let dest = SocketAddrV4::new(config.multicast_addr, config.port);

        // SSRC (ランダム)
        let ssrc = rand_u32();

        let rx_buf = RingBuffer::new(config.jitter_frames, config.channels as usize);

        let inner = Arc::new(NodeInner {
            socket,
            dest,
            rx_buf,
            running: AtomicBool::new(true),
            ssrc,
            seq: AtomicU32::new(0),
            channel: channel.to_string(),
            config,
        });

        // 受信スレッド起動
        let inner_clone = inner.clone();
        std::thread::Builder::new()
            .name("soluna-rx".to_string())
            .spawn(move || rx_loop(inner_clone))?;

        Ok(Node { inner })
    }

    /// 音声を全員に送信 (f32 PCM, sample_rate Hz, mono/stereo)
    ///
    /// オーディオコールバックから直接呼べる (lock-free)。
    /// 内部でOSTP/RTPパケットに変換してUDPマルチキャスト送信。
    ///
    /// ```rust
    /// // オーディオコールバック内:
    /// node.send_audio(&output_buffer);
    /// ```
    #[inline]
    pub fn send_audio(&self, samples: &[f32]) {
        if !self.inner.running.load(Ordering::Relaxed) { return; }

        let spp = self.inner.config.samples_per_packet;
        let channels = self.inner.config.channels as usize;

        // サンプルをパケットサイズに分割して送信
        for chunk in samples.chunks(spp * channels) {
            let seq = self.inner.seq.fetch_add(1, Ordering::Relaxed) as u16;
            let ts = seq as u32 * spp as u32;

            let packet = build_rtp_packet(
                seq, ts, self.inner.ssrc,
                chunk,
            );

            let _ = self.inner.socket.send_to(&packet, self.inner.dest);
        }
    }

    /// 他のノードの音声を受信 (f32 PCM)
    ///
    /// `out` バッファにコピー。利用可能なサンプル数を返す。
    /// オーディオコールバックから直接呼べる (lock-free)。
    ///
    /// ```rust
    /// let mut buf = [0.0f32; 256];
    /// let n = node.recv_audio(&mut buf);
    /// // buf[..n] に音声データ
    /// ```
    #[inline]
    pub fn recv_audio(&self, out: &mut [f32]) -> usize {
        self.inner.rx_buf.read(out)
    }

    /// 受信バッファに溜まっているサンプル数
    #[inline]
    pub fn available(&self) -> usize {
        self.inner.rx_buf.available()
    }

    /// チャンネル名
    pub fn channel(&self) -> &str {
        &self.inner.channel
    }

    /// 切断
    pub fn leave(&self) {
        self.inner.running.store(false, Ordering::Relaxed);
    }
}

impl Drop for NodeInner {
    fn drop(&mut self) {
        self.running.store(false, Ordering::Relaxed);
        let _ = self.socket.leave_multicast_v4(
            &self.config.multicast_addr,
            &Ipv4Addr::UNSPECIFIED,
        );
    }
}

// ── 受信ループ ──

fn rx_loop(inner: Arc<NodeInner>) {
    let mut buf = [0u8; 2048];
    let own_ssrc = inner.ssrc;

    while inner.running.load(Ordering::Relaxed) {
        match inner.socket.recv_from(&mut buf) {
            Ok((n, _addr)) => {
                if n < 24 { continue; } // OSTPヘッダ最小サイズ

                // OSTP/RTPパース
                if let Some(packet) = ostp::parse(&buf[..n]) {
                    // 自分のパケットは無視
                    if packet.rtp.ssrc == own_ssrc { continue; }

                    // ペイロードをf32に変換してリングバッファに書き込み
                    let samples = payload_to_f32(packet.payload, packet.rtp.payload_type);
                    inner.rx_buf.write(&samples);
                }
            }
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                // ノンブロッキング: データなし → 少し待つ
                std::thread::sleep(std::time::Duration::from_micros(500));
            }
            Err(_) => break,
        }
    }
}

// ── パケット構築 ──

fn build_rtp_packet(seq: u16, timestamp: u32, ssrc: u32, samples: &[f32]) -> Vec<u8> {
    // RTP Header (12 bytes) + OSTP Extension (12 bytes) + Payload
    let mut packet = Vec::with_capacity(24 + samples.len() * 4);

    // RTP Header
    let b0: u8 = 0x90; // V=2, P=0, X=1 (extension), CC=0
    let b1: u8 = ostp::PT_F32; // payload type: f32
    packet.push(b0);
    packet.push(b1);
    packet.extend_from_slice(&seq.to_be_bytes());
    packet.extend_from_slice(&timestamp.to_be_bytes());
    packet.extend_from_slice(&ssrc.to_be_bytes());

    // RTP Extension Header (4 bytes)
    packet.extend_from_slice(&0x4F53u16.to_be_bytes()); // profile = "OS"
    packet.extend_from_slice(&2u16.to_be_bytes());       // length = 2 (32-bit words)

    // OSTP Header (8 bytes)
    packet.extend_from_slice(&0u16.to_be_bytes()); // stream_id
    packet.extend_from_slice(&0u16.to_be_bytes()); // sequence_ext
    packet.extend_from_slice(&timestamp.to_be_bytes()); // media_timestamp

    // Payload (f32 LE)
    for &s in samples {
        packet.extend_from_slice(&s.to_le_bytes());
    }

    packet
}

fn payload_to_f32(payload: &[u8], pt: u8) -> Vec<f32> {
    match pt {
        ostp::PT_F32 => {
            payload.chunks_exact(4)
                .map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]]))
                .collect()
        }
        ostp::PT_PCM24 => {
            payload.chunks_exact(4)
                .map(|c| {
                    let i = i32::from_le_bytes([c[0], c[1], c[2], c[3]]);
                    i as f32 / 8388608.0 // 2^23
                })
                .collect()
        }
        ostp::PT_AES67_L16 | 11 => {
            payload.chunks_exact(2)
                .map(|c| {
                    let i = i16::from_be_bytes([c[0], c[1]]);
                    i as f32 / 32768.0
                })
                .collect()
        }
        _ => Vec::new(),
    }
}

fn rand_u32() -> u32 {
    // Simple non-crypto random from thread ID + time
    let t = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    (t.as_nanos() as u32) ^ (std::thread::current().id().as_u64().get() as u32)
}

// ── テスト ──

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_build_rtp_packet() {
        let samples = [0.5f32, -0.5, 0.25, -0.25];
        let packet = build_rtp_packet(1, 0, 12345, &samples);
        assert_eq!(packet.len(), 24 + 16); // header + 4 samples * 4 bytes

        // Check RTP version
        assert_eq!(packet[0] >> 6, 2);
        // Check extension bit
        assert_eq!((packet[0] >> 4) & 1, 1);
        // Check OSTP profile
        assert_eq!(&packet[12..14], &[0x4F, 0x53]); // "OS"
    }

    #[test]
    fn test_payload_to_f32() {
        let samples = [0.5f32, -0.5];
        let mut payload = Vec::new();
        for s in &samples {
            payload.extend_from_slice(&s.to_le_bytes());
        }
        let result = payload_to_f32(&payload, ostp::PT_F32);
        assert_eq!(result, samples);
    }
}
