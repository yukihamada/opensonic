//! Audio Integrity Verification — 音声が正しく届いたか検証
//!
//! 送信側と受信側で音声データのフィンガープリントを比較し、
//! エンドツーエンドの音声完全性を検証する。
//!
//! # 3つの検証レベル
//!
//! 1. **Packet-level**: CRC-32 (OSTP trailer) — パケット単位の完全性
//! 2. **Sequence-level**: シーケンス番号追跡 — 欠落・重複・順序乱れ検出
//! 3. **Audio-level**: 音声フィンガープリント — 内容の意味的一致検証

use std::collections::VecDeque;

// ── 1. Sequence Verification ──

/// パケットシーケンス追跡と欠落検出
pub struct SequenceVerifier {
    /// 最後に受信したシーケンス番号
    last_seq: Option<u16>,
    /// 受信済みパケット数
    received: u64,
    /// 欠落パケット数
    lost: u64,
    /// 重複パケット数
    duplicates: u64,
    /// 順序乱れ回数
    out_of_order: u64,
    /// 直近のシーケンス番号（重複検出用）
    recent_seqs: VecDeque<u16>,
}

impl SequenceVerifier {
    pub fn new() -> Self {
        Self {
            last_seq: None,
            received: 0,
            lost: 0,
            duplicates: 0,
            out_of_order: 0,
            recent_seqs: VecDeque::with_capacity(256),
        }
    }

    /// パケットを受信した時に呼ぶ。検証結果を返す。
    pub fn verify_packet(&mut self, seq: u16) -> PacketVerdict {
        // 重複チェック
        if self.recent_seqs.contains(&seq) {
            self.duplicates += 1;
            return PacketVerdict::Duplicate;
        }

        // リングバッファに追加
        if self.recent_seqs.len() >= 256 {
            self.recent_seqs.pop_front();
        }
        self.recent_seqs.push_back(seq);

        self.received += 1;

        let verdict = match self.last_seq {
            None => PacketVerdict::Ok, // 最初のパケット
            Some(last) => {
                let expected = last.wrapping_add(1);
                if seq == expected {
                    PacketVerdict::Ok
                } else if seq_gt(seq, expected) {
                    // ギャップ（欠落）
                    let gap = seq_diff(expected, seq);
                    self.lost += gap as u64;
                    PacketVerdict::Gap { missing: gap }
                } else {
                    // 順序乱れ（遅延到着）
                    self.out_of_order += 1;
                    PacketVerdict::OutOfOrder
                }
            }
        };

        if !matches!(verdict, PacketVerdict::OutOfOrder) {
            self.last_seq = Some(seq);
        }

        verdict
    }

    /// 検証統計
    pub fn stats(&self) -> SequenceStats {
        let total = self.received + self.lost;
        SequenceStats {
            received: self.received,
            lost: self.lost,
            duplicates: self.duplicates,
            out_of_order: self.out_of_order,
            loss_rate: if total > 0 { self.lost as f64 / total as f64 * 100.0 } else { 0.0 },
        }
    }

    /// リセット
    pub fn reset(&mut self) {
        *self = Self::new();
    }
}

impl Default for SequenceVerifier {
    fn default() -> Self {
        Self::new()
    }
}

/// パケット検証結果
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PacketVerdict {
    /// 正常受信
    Ok,
    /// パケット欠落あり
    Gap { missing: u16 },
    /// 重複パケット
    Duplicate,
    /// 順序乱れ（遅延到着）
    OutOfOrder,
}

/// シーケンス統計
#[derive(Debug, Clone)]
pub struct SequenceStats {
    pub received: u64,
    pub lost: u64,
    pub duplicates: u64,
    pub out_of_order: u64,
    pub loss_rate: f64,
}

impl std::fmt::Display for SequenceStats {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f,
            "recv={} lost={} dup={} ooo={} loss={:.2}%",
            self.received, self.lost, self.duplicates, self.out_of_order, self.loss_rate,
        )
    }
}

// ── 2. Audio Fingerprint ──

/// 音声フィンガープリント — 内容の意味的一致を検証
///
/// エネルギーベースのフィンガープリントを使用。
/// PCM データ全体のハッシュではなく、周波数帯域ごとのエネルギーを
/// 量子化してハッシュすることで、軽微なジッターや丸め誤差に耐性を持つ。
pub struct AudioFingerprint {
    /// フレームごとのエネルギーハッシュ
    frames: Vec<u64>,
}

impl AudioFingerprint {
    /// f32 PCM サンプルからフィンガープリントを生成
    ///
    /// `frame_size` サンプルごとにエネルギーを計算してハッシュ。
    pub fn from_samples(samples: &[f32], frame_size: usize) -> Self {
        let frames = samples.chunks(frame_size)
            .map(|frame| Self::frame_hash(frame))
            .collect();
        Self { frames }
    }

    /// 2つのフィンガープリントの類似度を計算 (0.0 - 1.0)
    ///
    /// 1.0 = 完全一致, 0.0 = 完全不一致
    pub fn similarity(&self, other: &AudioFingerprint) -> f64 {
        if self.frames.is_empty() || other.frames.is_empty() {
            return 0.0;
        }

        let len = self.frames.len().min(other.frames.len());
        let mut matches = 0usize;

        for i in 0..len {
            // ハミング距離が近いフレームをマッチとみなす
            let hamming = (self.frames[i] ^ other.frames[i]).count_ones();
            if hamming <= 8 { // 64ビット中8ビット以下の差 = 87.5%一致
                matches += 1;
            }
        }

        matches as f64 / len as f64
    }

    /// 完全一致かチェック
    pub fn is_exact_match(&self, other: &AudioFingerprint) -> bool {
        self.frames == other.frames
    }

    /// フレーム数
    pub fn len(&self) -> usize {
        self.frames.len()
    }

    /// 空か
    pub fn is_empty(&self) -> bool {
        self.frames.is_empty()
    }

    /// フレームのエネルギーハッシュ
    fn frame_hash(frame: &[f32]) -> u64 {
        if frame.is_empty() {
            return 0;
        }

        // フレームを8帯域に分割し、各帯域のエネルギーを計算
        let band_size = (frame.len() / 8).max(1);
        let mut hash: u64 = 0;

        for band in 0..8 {
            let start = band * band_size;
            let end = ((band + 1) * band_size).min(frame.len());
            if start >= frame.len() { break; }

            let energy: f32 = frame[start..end].iter()
                .map(|s| s * s)
                .sum();

            // エネルギーを8ビットに量子化
            let quantized = Self::quantize_energy(energy, (end - start) as f32);
            hash |= (quantized as u64) << (band * 8);
        }

        hash
    }

    /// エネルギーを8ビットに量子化（対数スケール）
    fn quantize_energy(energy: f32, len: f32) -> u8 {
        let rms = (energy / len).sqrt();
        // -60dB to 0dB を 0-255 にマッピング
        let db = if rms > 0.0 { 20.0 * rms.log10() } else { -60.0 };
        let normalized = ((db + 60.0) / 60.0).clamp(0.0, 1.0);
        (normalized * 255.0) as u8
    }
}

// ── 3. End-to-End Verifier ──

/// エンドツーエンド音声検証
///
/// 送信側と受信側の両方で使用。
pub struct AudioVerifier {
    pub sequence: SequenceVerifier,
    /// 送信/受信した音声のフィンガープリント蓄積用バッファ
    sample_buffer: Vec<f32>,
    /// フィンガープリント用フレームサイズ
    frame_size: usize,
    /// 生成されたフィンガープリント
    fingerprints: Vec<AudioFingerprint>,
    /// フィンガープリント生成間隔（サンプル数）
    fingerprint_interval: usize,
}

impl AudioVerifier {
    /// 新しいVerifierを作成
    ///
    /// `fingerprint_interval_secs`: フィンガープリント生成間隔（秒）
    pub fn new(sample_rate: u32, fingerprint_interval_secs: u32) -> Self {
        let frame_size = sample_rate as usize / 10; // 100ms フレーム
        Self {
            sequence: SequenceVerifier::new(),
            sample_buffer: Vec::new(),
            frame_size,
            fingerprints: Vec::new(),
            fingerprint_interval: sample_rate as usize * fingerprint_interval_secs as usize,
        }
    }

    /// 音声サンプルを記録（送信側 or 受信側）
    pub fn record_samples(&mut self, samples: &[f32]) {
        self.sample_buffer.extend_from_slice(samples);

        // 一定量たまったらフィンガープリント生成
        if self.sample_buffer.len() >= self.fingerprint_interval {
            let fp = AudioFingerprint::from_samples(&self.sample_buffer, self.frame_size);
            self.fingerprints.push(fp);
            self.sample_buffer.clear();
        }
    }

    /// パケットを検証
    pub fn verify_packet(&mut self, seq: u16) -> PacketVerdict {
        self.sequence.verify_packet(seq)
    }

    /// 送信側のフィンガープリントと比較
    pub fn compare_with(&self, sender: &AudioVerifier) -> VerificationReport {
        let seq_stats = self.sequence.stats();

        let len = self.fingerprints.len().min(sender.fingerprints.len());
        let mut similarities = Vec::with_capacity(len);

        for i in 0..len {
            similarities.push(self.fingerprints[i].similarity(&sender.fingerprints[i]));
        }

        let avg_similarity = if similarities.is_empty() {
            0.0
        } else {
            similarities.iter().sum::<f64>() / similarities.len() as f64
        };

        let min_similarity = similarities.iter().copied()
            .min_by(|a, b| a.partial_cmp(b).unwrap())
            .unwrap_or(0.0);

        VerificationReport {
            is_verified: avg_similarity > 0.85 && seq_stats.loss_rate < 5.0,
            sequence_stats: seq_stats,
            fingerprint_segments: len,
            avg_similarity,
            min_similarity,
        }
    }

    /// フィンガープリント数
    pub fn fingerprint_count(&self) -> usize {
        self.fingerprints.len()
    }
}

/// 検証レポート
#[derive(Debug, Clone)]
pub struct VerificationReport {
    pub sequence_stats: SequenceStats,
    pub fingerprint_segments: usize,
    pub avg_similarity: f64,
    pub min_similarity: f64,
    pub is_verified: bool,
}

impl std::fmt::Display for VerificationReport {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f,
            "verified={} | seq: {} | audio: {}/{} segments, similarity avg={:.1}% min={:.1}%",
            if self.is_verified { "YES" } else { "NO" },
            self.sequence_stats,
            self.fingerprint_segments, self.fingerprint_segments,
            self.avg_similarity * 100.0,
            self.min_similarity * 100.0,
        )
    }
}

// ── Helper: RTP sequence number arithmetic (mod 2^16) ──

/// seq_a > seq_b (with wraparound)
fn seq_gt(a: u16, b: u16) -> bool {
    let diff = a.wrapping_sub(b);
    diff > 0 && diff < 32768
}

/// Distance from a to b (forward, with wraparound)
fn seq_diff(from: u16, to: u16) -> u16 {
    to.wrapping_sub(from)
}

#[cfg(test)]
mod tests {
    use super::*;

    // ── Sequence Verifier ──

    #[test]
    fn test_sequential_packets() {
        let mut v = SequenceVerifier::new();
        assert_eq!(v.verify_packet(0), PacketVerdict::Ok);
        assert_eq!(v.verify_packet(1), PacketVerdict::Ok);
        assert_eq!(v.verify_packet(2), PacketVerdict::Ok);

        let stats = v.stats();
        assert_eq!(stats.received, 3);
        assert_eq!(stats.lost, 0);
    }

    #[test]
    fn test_packet_loss_detection() {
        let mut v = SequenceVerifier::new();
        v.verify_packet(0);
        v.verify_packet(1);
        // Skip 2, 3
        let verdict = v.verify_packet(4);
        assert_eq!(verdict, PacketVerdict::Gap { missing: 2 });

        let stats = v.stats();
        assert_eq!(stats.lost, 2);
    }

    #[test]
    fn test_duplicate_detection() {
        let mut v = SequenceVerifier::new();
        v.verify_packet(0);
        v.verify_packet(1);
        let verdict = v.verify_packet(1); // duplicate!
        assert_eq!(verdict, PacketVerdict::Duplicate);

        let stats = v.stats();
        assert_eq!(stats.duplicates, 1);
    }

    #[test]
    fn test_out_of_order() {
        let mut v = SequenceVerifier::new();
        v.verify_packet(0);
        v.verify_packet(2); // skip 1
        let verdict = v.verify_packet(1); // late arrival
        assert_eq!(verdict, PacketVerdict::OutOfOrder);
    }

    #[test]
    fn test_sequence_wraparound() {
        let mut v = SequenceVerifier::new();
        v.verify_packet(65534);
        v.verify_packet(65535);
        let verdict = v.verify_packet(0); // wraparound
        assert_eq!(verdict, PacketVerdict::Ok);
    }

    // ── Audio Fingerprint ──

    #[test]
    fn test_fingerprint_identical() {
        let samples: Vec<f32> = (0..4800).map(|i| (i as f32 * 0.01).sin()).collect();
        let fp1 = AudioFingerprint::from_samples(&samples, 480);
        let fp2 = AudioFingerprint::from_samples(&samples, 480);

        assert!(fp1.is_exact_match(&fp2));
        assert_eq!(fp1.similarity(&fp2), 1.0);
    }

    #[test]
    fn test_fingerprint_similar() {
        let samples1: Vec<f32> = (0..4800).map(|i| (i as f32 * 0.01).sin()).collect();
        // Slightly modified (small noise)
        let samples2: Vec<f32> = samples1.iter()
            .map(|s| s + 0.001)
            .collect();
        let fp1 = AudioFingerprint::from_samples(&samples1, 480);
        let fp2 = AudioFingerprint::from_samples(&samples2, 480);

        assert!(fp1.similarity(&fp2) > 0.9, "Similar audio should have high similarity");
    }

    #[test]
    fn test_fingerprint_different() {
        let samples1: Vec<f32> = (0..4800).map(|i| (i as f32 * 0.01).sin()).collect();
        let samples2: Vec<f32> = (0..4800).map(|i| (i as f32 * 0.1).cos() * 0.5).collect();
        let fp1 = AudioFingerprint::from_samples(&samples1, 480);
        let fp2 = AudioFingerprint::from_samples(&samples2, 480);

        assert!(fp1.similarity(&fp2) < 0.8, "Different audio should have low similarity");
    }

    #[test]
    fn test_fingerprint_silence() {
        let silence = vec![0.0f32; 4800];
        let fp = AudioFingerprint::from_samples(&silence, 480);
        assert!(!fp.is_empty());
    }

    // ── End-to-End Verifier ──

    #[test]
    fn test_e2e_perfect_transmission() {
        let mut sender = AudioVerifier::new(48000, 1);
        let mut receiver = AudioVerifier::new(48000, 1);

        // Generate and "transmit" 2 seconds of audio
        let audio: Vec<f32> = (0..96000).map(|i| (i as f32 * 440.0 / 48000.0 * std::f32::consts::TAU).sin()).collect();

        // Sender records
        sender.record_samples(&audio);

        // Receiver gets same audio (perfect transmission) + seq verification
        for seq in 0..200u16 {
            receiver.verify_packet(seq);
        }
        receiver.record_samples(&audio);

        let report = receiver.compare_with(&sender);
        assert!(report.is_verified, "Perfect transmission should be verified: {report}");
        assert_eq!(report.avg_similarity, 1.0);
        assert_eq!(report.sequence_stats.lost, 0);
    }

    #[test]
    fn test_e2e_lossy_transmission() {
        let mut sender = AudioVerifier::new(48000, 1);
        let mut receiver = AudioVerifier::new(48000, 1);

        let audio: Vec<f32> = (0..96000).map(|i| (i as f32 * 440.0 / 48000.0 * std::f32::consts::TAU).sin()).collect();
        sender.record_samples(&audio);

        // Simulate 10% packet loss
        for seq in 0..200u16 {
            if seq % 10 != 0 { // Drop every 10th packet
                receiver.verify_packet(seq);
            }
        }
        receiver.record_samples(&audio);

        let report = receiver.compare_with(&sender);
        assert!(report.sequence_stats.loss_rate > 5.0);
    }

    #[test]
    fn test_seq_wraparound_arithmetic() {
        assert!(seq_gt(1, 0));
        assert!(seq_gt(0, 65535)); // wraparound
        assert!(!seq_gt(0, 1));

        assert_eq!(seq_diff(65534, 0), 2); // wraparound
        assert_eq!(seq_diff(0, 5), 5);
    }
}
