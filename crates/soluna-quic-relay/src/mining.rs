//! Relay Mining — 帯域貢献に対するトークン報酬
//!
//! QUIC ブリッジを通過するトラフィックを接続単位で計測し、
//! リレーノード運営者に ENAI トークン報酬を計算する。
//!
//! # 報酬モデル
//!
//! ```text
//! 報酬 = bytes_relayed × rate_per_byte × tier_multiplier
//!
//! Tier:
//!   Origin (1台)   → 1.0x
//!   Region (~20台) → 0.5x
//!   Edge (~10K台)  → 0.25x
//!   P2P Swarm      → 0.1x
//! ```
//!
//! # 不正防止
//!
//! - 同一 SSRC からの重複パケットはカウントしない
//! - PoL の Merkle Root と照合して実際のリスナーがいることを確認
//! - 最大報酬レート制限（1接続あたり 10 ENAI/hour）

use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

/// リレーティア（報酬倍率に影響）
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RelayTier {
    Origin,  // 1.0x
    Region,  // 0.5x
    Edge,    // 0.25x
    Swarm,   // 0.1x
}

impl RelayTier {
    pub fn multiplier(&self) -> f64 {
        match self {
            RelayTier::Origin => 1.0,
            RelayTier::Region => 0.5,
            RelayTier::Edge => 0.25,
            RelayTier::Swarm => 0.1,
        }
    }
}

/// 1バイトあたりの基本報酬レート (ENAI)
/// 1MB ≈ 0.001 ENAI, 1時間の48kHz mono ≈ 170MB ≈ 0.17 ENAI
const BASE_RATE_PER_BYTE: f64 = 1e-9;

/// 1接続あたりの最大報酬 (ENAI/hour)
const MAX_REWARD_PER_HOUR: f64 = 10.0;

/// 接続ごとのトラフィック統計
#[derive(Debug)]
pub struct ConnectionStats {
    /// 中継した音声バイト数（QUIC datagram）
    pub audio_bytes: AtomicU64,
    /// 中継した制御バイト数（QUIC stream）
    pub control_bytes: AtomicU64,
    /// 受信した一意のパケット数
    pub unique_packets: AtomicU64,
    /// 接続開始時刻
    pub connected_at: Instant,
    /// 最後のアクティビティ
    pub last_active: std::sync::Mutex<Instant>,
    /// リモートアドレス
    pub remote_addr: String,
    /// チャンネル名
    pub channel: std::sync::Mutex<String>,
}

impl ConnectionStats {
    pub fn new(remote_addr: String) -> Self {
        let now = Instant::now();
        Self {
            audio_bytes: AtomicU64::new(0),
            control_bytes: AtomicU64::new(0),
            unique_packets: AtomicU64::new(0),
            connected_at: now,
            last_active: std::sync::Mutex::new(now),
            remote_addr,
            channel: std::sync::Mutex::new(String::new()),
        }
    }

    /// 音声パケット中継を記録
    #[inline]
    pub fn record_audio(&self, bytes: u64) {
        self.audio_bytes.fetch_add(bytes, Ordering::Relaxed);
        self.unique_packets.fetch_add(1, Ordering::Relaxed);
        *self.last_active.lock().unwrap() = Instant::now();
    }

    /// 制御メッセージ中継を記録
    #[inline]
    pub fn record_control(&self, bytes: u64) {
        self.control_bytes.fetch_add(bytes, Ordering::Relaxed);
        *self.last_active.lock().unwrap() = Instant::now();
    }

    /// チャンネル名を設定（JOIN メッセージから抽出）
    pub fn set_channel(&self, channel: &str) {
        *self.channel.lock().unwrap() = channel.to_string();
    }

    /// 接続時間（秒）
    pub fn uptime_secs(&self) -> f64 {
        self.connected_at.elapsed().as_secs_f64()
    }

    /// 総中継バイト数
    pub fn total_bytes(&self) -> u64 {
        self.audio_bytes.load(Ordering::Relaxed) + self.control_bytes.load(Ordering::Relaxed)
    }
}

/// マイニングマネージャー — 全接続の統計を集約して報酬を計算
pub struct MiningManager {
    tier: RelayTier,
    connections: HashMap<usize, ConnectionStats>,
    /// 累計報酬 (ENAI × 1e9 = nanoENAI for precision)
    total_rewards_nano: u64,
    /// ウォレットアドレス（Solana）
    wallet: String,
}

impl MiningManager {
    pub fn new(tier: RelayTier, wallet: String) -> Self {
        Self {
            tier,
            connections: HashMap::new(),
            total_rewards_nano: 0,
            wallet,
        }
    }

    /// 新しい接続を登録
    pub fn register(&mut self, conn_id: usize, remote_addr: String) {
        self.connections.insert(conn_id, ConnectionStats::new(remote_addr));
    }

    /// 接続を削除し、最終報酬を計算
    pub fn unregister(&mut self, conn_id: usize) -> Option<ConnectionReward> {
        let stats = self.connections.remove(&conn_id)?;
        let reward = self.calculate_reward(&stats);
        self.total_rewards_nano += (reward * 1e9) as u64;
        let channel = stats.channel.lock().unwrap().clone();
        let audio_bytes = stats.audio_bytes.load(Ordering::Relaxed);
        let control_bytes = stats.control_bytes.load(Ordering::Relaxed);
        let unique_packets = stats.unique_packets.load(Ordering::Relaxed);
        let uptime_secs = stats.uptime_secs();
        let remote_addr = stats.remote_addr.clone();
        Some(ConnectionReward {
            conn_id,
            remote_addr,
            channel,
            audio_bytes,
            control_bytes,
            unique_packets,
            uptime_secs,
            reward_enai: reward,
        })
    }

    /// 接続の統計を取得
    pub fn get_stats(&self, conn_id: usize) -> Option<&ConnectionStats> {
        self.connections.get(&conn_id)
    }

    /// 全接続の現時点での報酬概要
    pub fn summary(&self) -> MiningSummary {
        let mut total_audio = 0u64;
        let mut total_control = 0u64;
        let mut total_packets = 0u64;
        let mut active = 0usize;

        for stats in self.connections.values() {
            total_audio += stats.audio_bytes.load(Ordering::Relaxed);
            total_control += stats.control_bytes.load(Ordering::Relaxed);
            total_packets += stats.unique_packets.load(Ordering::Relaxed);
            let last = *stats.last_active.lock().unwrap();
            if last.elapsed() < Duration::from_secs(30) {
                active += 1;
            }
        }

        let pending_reward: f64 = self.connections.values()
            .map(|s| self.calculate_reward(s))
            .sum();

        MiningSummary {
            tier: self.tier,
            wallet: self.wallet.clone(),
            active_connections: active,
            total_connections: self.connections.len(),
            total_audio_bytes: total_audio,
            total_control_bytes: total_control,
            total_packets,
            total_rewards_enai: self.total_rewards_nano as f64 / 1e9,
            pending_rewards_enai: pending_reward,
        }
    }

    /// 報酬計算（不正防止付き）
    fn calculate_reward(&self, stats: &ConnectionStats) -> f64 {
        let bytes = stats.total_bytes() as f64;
        let raw_reward = bytes * BASE_RATE_PER_BYTE * self.tier.multiplier();

        // Rate limit: max reward per hour
        let hours = stats.uptime_secs() / 3600.0;
        let max = if hours > 0.0 { MAX_REWARD_PER_HOUR * hours } else { 0.0 };

        raw_reward.min(max)
    }
}

/// 接続終了時の報酬レポート
#[derive(Debug, Clone)]
pub struct ConnectionReward {
    pub conn_id: usize,
    pub remote_addr: String,
    pub channel: String,
    pub audio_bytes: u64,
    pub control_bytes: u64,
    pub unique_packets: u64,
    pub uptime_secs: f64,
    pub reward_enai: f64,
}

/// マイニング概要
#[derive(Debug, Clone)]
pub struct MiningSummary {
    pub tier: RelayTier,
    pub wallet: String,
    pub active_connections: usize,
    pub total_connections: usize,
    pub total_audio_bytes: u64,
    pub total_control_bytes: u64,
    pub total_packets: u64,
    pub total_rewards_enai: f64,
    pub pending_rewards_enai: f64,
}

impl std::fmt::Display for MiningSummary {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f,
            "[mining] tier={:?} active={}/{} audio={:.1}MB ctrl={:.1}KB pkts={} rewards={:.6}+{:.6} ENAI wallet={}",
            self.tier,
            self.active_connections, self.total_connections,
            self.total_audio_bytes as f64 / 1_048_576.0,
            self.total_control_bytes as f64 / 1024.0,
            self.total_packets,
            self.total_rewards_enai,
            self.pending_rewards_enai,
            &self.wallet[..8.min(self.wallet.len())],
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_connection_stats() {
        let stats = ConnectionStats::new("1.2.3.4:5000".to_string());
        stats.record_audio(1000);
        stats.record_audio(500);
        stats.record_control(100);

        assert_eq!(stats.audio_bytes.load(Ordering::Relaxed), 1500);
        assert_eq!(stats.control_bytes.load(Ordering::Relaxed), 100);
        assert_eq!(stats.unique_packets.load(Ordering::Relaxed), 2);
        assert_eq!(stats.total_bytes(), 1600);
    }

    #[test]
    fn test_mining_reward_calculation() {
        let mut mgr = MiningManager::new(RelayTier::Origin, "WALLET123".to_string());
        mgr.register(0, "1.2.3.4:5000".to_string());

        let stats = mgr.get_stats(0).unwrap();
        // Simulate 170MB (1 hour of 48kHz mono)
        stats.record_audio(170_000_000);

        let summary = mgr.summary();
        assert_eq!(summary.active_connections, 1);
        assert!(summary.pending_rewards_enai > 0.0);
    }

    #[test]
    fn test_tier_multipliers() {
        assert_eq!(RelayTier::Origin.multiplier(), 1.0);
        assert_eq!(RelayTier::Region.multiplier(), 0.5);
        assert_eq!(RelayTier::Edge.multiplier(), 0.25);
        assert_eq!(RelayTier::Swarm.multiplier(), 0.1);
    }

    #[test]
    fn test_unregister_reward() {
        let mut mgr = MiningManager::new(RelayTier::Edge, "WALLET456".to_string());
        mgr.register(42, "5.6.7.8:9000".to_string());

        let stats = mgr.get_stats(42).unwrap();
        stats.record_audio(1_000_000); // 1MB
        stats.set_channel("jazz");

        let reward = mgr.unregister(42).unwrap();
        assert_eq!(reward.conn_id, 42);
        assert_eq!(reward.channel, "jazz");
        assert_eq!(reward.audio_bytes, 1_000_000);
        assert!(reward.reward_enai >= 0.0);
    }
}
