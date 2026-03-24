//! Adaptive Bitrate + Transport Selection + Swarm Intelligence
//!
//! 3つの適応型機能:
//! 1. **Adaptive Bitrate**: パケットロス率に基づくOpus bitrate自動切替
//! 2. **Transport Fallback**: LAN→P2P→QUIC→WebSocket の自動選択
//! 3. **Swarm Intelligence**: ノード状態に基づく動的ツリー再構築

use std::time::{Duration, Instant};
use std::collections::VecDeque;

// ── 1. Adaptive Bitrate ──

/// Opus ビットレート階層
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum BitrateLevel {
    /// 32 kbps — 極低帯域・衛星接続向け
    Ultra = 32_000,
    /// 64 kbps — モバイル4G/混雑WiFi向け
    Low = 64_000,
    /// 128 kbps — 標準品質（デフォルト）
    Standard = 128_000,
    /// 256 kbps — 高品質・光回線向け
    High = 256_000,
}

impl BitrateLevel {
    pub fn bitrate(&self) -> u32 {
        *self as u32
    }

    /// パケットロス率からビットレートを決定
    pub fn from_loss_rate(loss_pct: f32) -> Self {
        match loss_pct {
            l if l < 1.0 => BitrateLevel::High,
            l if l < 5.0 => BitrateLevel::Standard,
            l if l < 15.0 => BitrateLevel::Low,
            _ => BitrateLevel::Ultra,
        }
    }

    /// FEC (Forward Error Correction) のオーバーヘッド率
    pub fn fec_overhead(&self) -> f32 {
        match self {
            BitrateLevel::High => 0.0,      // FEC なし
            BitrateLevel::Standard => 0.1,   // 10% FEC
            BitrateLevel::Low => 0.25,       // 25% FEC
            BitrateLevel::Ultra => 0.5,      // 50% FEC（強力）
        }
    }

    /// Opus パケットサイズ（サンプル数 @ 48kHz）
    pub fn samples_per_packet(&self) -> usize {
        match self {
            BitrateLevel::High => 240,     // 5ms (低遅延)
            BitrateLevel::Standard => 480, // 10ms
            BitrateLevel::Low => 960,      // 20ms (帯域効率)
            BitrateLevel::Ultra => 1920,   // 40ms (最大効率)
        }
    }
}

/// パケットロス率を追跡するスライディングウィンドウ
pub struct LossTracker {
    /// (timestamp, was_received) のリングバッファ
    window: VecDeque<(Instant, bool)>,
    /// ウィンドウサイズ
    window_duration: Duration,
    /// 現在のビットレートレベル
    current_level: BitrateLevel,
    /// 最後にレベル変更した時刻（頻繁な切替防止）
    pub last_change: Instant,
    /// レベル変更の最小間隔
    pub change_interval: Duration,
    /// 予測用: 直近のロス率推移
    loss_history: VecDeque<f32>,
}

impl LossTracker {
    pub fn new() -> Self {
        Self {
            window: VecDeque::with_capacity(1000),
            window_duration: Duration::from_secs(5),
            current_level: BitrateLevel::Standard,
            last_change: Instant::now(),
            change_interval: Duration::from_secs(3), // 最低3秒は同じレベル維持
            loss_history: VecDeque::with_capacity(60),
        }
    }

    /// パケット受信を記録
    pub fn record_received(&mut self) {
        self.window.push_back((Instant::now(), true));
        self.cleanup();
    }

    /// パケットロスを記録（シーケンス番号のギャップから検出）
    pub fn record_lost(&mut self, count: u32) {
        let now = Instant::now();
        for _ in 0..count {
            self.window.push_back((now, false));
        }
        self.cleanup();
    }

    /// 現在のロス率 (%)
    pub fn loss_rate(&self) -> f32 {
        if self.window.is_empty() {
            return 0.0;
        }
        let lost = self.window.iter().filter(|(_, r)| !r).count();
        (lost as f32 / self.window.len() as f32) * 100.0
    }

    /// ビットレートレベルを更新。変更があれば新レベルを返す。
    pub fn update(&mut self) -> Option<BitrateLevel> {
        let loss = self.loss_rate();
        self.loss_history.push_back(loss);
        if self.loss_history.len() > 60 {
            self.loss_history.pop_front();
        }

        let target = BitrateLevel::from_loss_rate(loss);

        // 予測: ロス率が上昇トレンドなら早めにダウングレード
        let trend = self.loss_trend();
        let adjusted_target = if trend > 2.0 && target > BitrateLevel::Ultra {
            // ロス率が急上昇 → 1段下げる
            match target {
                BitrateLevel::High => BitrateLevel::Standard,
                BitrateLevel::Standard => BitrateLevel::Low,
                BitrateLevel::Low => BitrateLevel::Ultra,
                BitrateLevel::Ultra => BitrateLevel::Ultra,
            }
        } else {
            target
        };

        // ヒステリシス: ダウングレードは即座、アップグレードは慎重
        let should_change = if adjusted_target < self.current_level {
            // ダウングレード: すぐ
            self.last_change.elapsed() >= Duration::from_secs(1)
        } else if adjusted_target > self.current_level {
            // アップグレード: 安定してから
            self.last_change.elapsed() >= self.change_interval
        } else {
            false
        };

        if should_change {
            self.current_level = adjusted_target;
            self.last_change = Instant::now();
            Some(adjusted_target)
        } else {
            None
        }
    }

    /// 現在のビットレートレベル
    pub fn current_level(&self) -> BitrateLevel {
        self.current_level
    }

    /// ロス率の変化トレンド（正=悪化、負=改善）
    fn loss_trend(&self) -> f32 {
        if self.loss_history.len() < 4 {
            return 0.0;
        }
        let recent: f32 = self.loss_history.iter().rev().take(3).sum::<f32>() / 3.0;
        let older: f32 = self.loss_history.iter().rev().skip(3).take(3).sum::<f32>() / 3.0;
        recent - older
    }

    fn cleanup(&mut self) {
        let cutoff = Instant::now() - self.window_duration;
        while let Some(&(ts, _)) = self.window.front() {
            if ts < cutoff {
                self.window.pop_front();
            } else {
                break;
            }
        }
    }
}

impl Default for LossTracker {
    fn default() -> Self {
        Self::new()
    }
}

// ── 2. Transport Fallback ──

/// 利用可能なトランスポートモード
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TransportMode {
    /// UDP Multicast (LAN内、最低遅延)
    LanMulticast,
    /// UDP P2P (STUN hole-punch成功)
    P2pDirect,
    /// QUIC Unreliable Datagram (WAN、暗号化)
    Quic,
    /// WebSocket (ファイアウォール越え、フォールバック)
    WebSocket,
    /// HTTP Progressive (最終手段)
    HttpProgressive,
}

impl TransportMode {
    /// 優先順位（低い方が優先）
    pub fn priority(&self) -> u8 {
        match self {
            TransportMode::LanMulticast => 0,
            TransportMode::P2pDirect => 1,
            TransportMode::Quic => 2,
            TransportMode::WebSocket => 3,
            TransportMode::HttpProgressive => 4,
        }
    }

    /// 期待レイテンシー (ms)
    pub fn expected_latency_ms(&self) -> u32 {
        match self {
            TransportMode::LanMulticast => 1,
            TransportMode::P2pDirect => 15,
            TransportMode::Quic => 30,
            TransportMode::WebSocket => 80,
            TransportMode::HttpProgressive => 2000,
        }
    }

    /// 暗号化されているか
    pub fn is_encrypted(&self) -> bool {
        matches!(self, TransportMode::Quic | TransportMode::WebSocket | TransportMode::HttpProgressive)
    }
}

/// トランスポート候補の接続テスト結果
#[derive(Debug, Clone)]
pub struct ProbeResult {
    pub mode: TransportMode,
    pub success: bool,
    pub latency_ms: Option<u32>,
    pub timestamp: Instant,
}

/// トランスポート自動選択エンジン
pub struct TransportSelector {
    /// プローブ結果
    probes: Vec<ProbeResult>,
    /// 現在のモード
    current: TransportMode,
    /// 各モードの失敗回数
    failures: [u32; 5],
    /// 再プローブ間隔
    reprobe_interval: Duration,
    /// 最後のプローブ時刻
    last_probe: Instant,
}

impl TransportSelector {
    pub fn new() -> Self {
        Self {
            probes: Vec::new(),
            current: TransportMode::Quic, // デフォルト
            failures: [0; 5],
            reprobe_interval: Duration::from_secs(60),
            last_probe: Instant::now(),
        }
    }

    /// プローブ結果を記録
    pub fn record_probe(&mut self, result: ProbeResult) {
        if !result.success {
            self.failures[result.mode.priority() as usize] += 1;
        }
        self.probes.push(result);
    }

    /// 最適なトランスポートを選択
    pub fn select_best(&mut self) -> TransportMode {
        // 成功したプローブの中から最も優先度が高いものを選択
        let mut best: Option<(TransportMode, u32)> = None;

        for probe in self.probes.iter().rev() {
            if !probe.success { continue; }
            if probe.timestamp.elapsed() > Duration::from_secs(300) { continue; }

            let latency = probe.latency_ms.unwrap_or(999);
            match best {
                None => best = Some((probe.mode, latency)),
                Some((_, best_lat)) => {
                    // 優先度が高い or 同優先度でレイテンシーが低い
                    if probe.mode.priority() < best.unwrap().0.priority()
                        || (probe.mode.priority() == best.unwrap().0.priority() && latency < best_lat)
                    {
                        best = Some((probe.mode, latency));
                    }
                }
            }
        }

        if let Some((mode, _)) = best {
            self.current = mode;
        }

        self.current
    }

    /// 現在のモード
    pub fn current(&self) -> TransportMode {
        self.current
    }

    /// 再プローブが必要か
    pub fn needs_reprobe(&self) -> bool {
        self.last_probe.elapsed() >= self.reprobe_interval
    }

    /// 再プローブ開始を記録
    pub fn start_reprobe(&mut self) {
        self.last_probe = Instant::now();
        // 古いプローブ結果をクリーンアップ
        let cutoff = Instant::now() - Duration::from_secs(600);
        self.probes.retain(|p| p.timestamp > cutoff);
    }

    /// フォールバック: 現在のモードが失敗した場合に次を試す
    pub fn fallback(&mut self) -> TransportMode {
        let next = match self.current {
            TransportMode::LanMulticast => TransportMode::P2pDirect,
            TransportMode::P2pDirect => TransportMode::Quic,
            TransportMode::Quic => TransportMode::WebSocket,
            TransportMode::WebSocket => TransportMode::HttpProgressive,
            TransportMode::HttpProgressive => TransportMode::HttpProgressive, // 最終手段
        };
        self.current = next;
        next
    }
}

impl Default for TransportSelector {
    fn default() -> Self {
        Self::new()
    }
}

// ── 3. Swarm Intelligence ──

/// ノードの状態レポート
#[derive(Debug, Clone)]
pub struct NodeReport {
    /// ノード識別子
    pub node_id: u32,
    /// ネットワーク品質スコア (0-100, 100=最高)
    pub quality_score: u8,
    /// 利用可能帯域 (kbps)
    pub bandwidth_kbps: u32,
    /// 現在のパケットロス率 (%)
    pub loss_rate: f32,
    /// RTT (ms)
    pub rtt_ms: u32,
    /// 中継可能な接続数
    pub relay_capacity: u16,
    /// 現在の子ノード数
    pub current_children: u16,
    /// バッテリー残量 (0-100, 255=AC電源)
    pub battery: u8,
    /// トランスポートモード
    pub transport: TransportMode,
    /// レポート時刻
    pub timestamp: Instant,
}

impl NodeReport {
    /// ノードの総合スコア (リレー親として適切か)
    pub fn relay_fitness(&self) -> f64 {
        let bw_score = (self.bandwidth_kbps as f64 / 1000.0).min(10.0); // max 10
        let loss_penalty = self.loss_rate as f64 * 2.0; // ロス率のペナルティ
        let rtt_penalty = self.rtt_ms as f64 / 50.0; // RTTのペナルティ
        let capacity_score = if self.current_children < self.relay_capacity {
            (self.relay_capacity - self.current_children) as f64
        } else {
            -5.0 // 過負荷ペナルティ
        };
        let battery_score = if self.battery == 255 { 5.0 } // AC電源ボーナス
            else if self.battery > 50 { 2.0 }
            else if self.battery > 20 { 0.0 }
            else { -10.0 }; // 低バッテリーペナルティ

        bw_score + capacity_score + battery_score - loss_penalty - rtt_penalty
    }
}

/// スワームツリー再構築の判断
#[derive(Debug, Clone)]
pub enum SwarmAction {
    /// ツリーは最適、変更不要
    NoChange,
    /// ノードを別の親に再割当て
    Reassign { node_id: u32, new_parent_id: u32 },
    /// ノードをリレーに昇格
    PromoteToRelay { node_id: u32 },
    /// ノードをリレーから降格（バッテリー低下等）
    DemoteFromRelay { node_id: u32 },
    /// ビットレートを調整
    AdjustBitrate { node_id: u32, level: BitrateLevel },
}

/// スワーム最適化エンジン
pub struct SwarmOptimizer {
    /// 全ノードの最新レポート
    reports: Vec<NodeReport>,
    /// 最適化実行間隔
    optimize_interval: Duration,
    /// 最後の最適化時刻
    last_optimize: Instant,
}

impl SwarmOptimizer {
    pub fn new() -> Self {
        Self {
            reports: Vec::new(),
            optimize_interval: Duration::from_secs(10),
            last_optimize: Instant::now(),
        }
    }

    /// ノードレポートを受信
    pub fn update_node(&mut self, report: NodeReport) {
        // 既存のレポートを更新
        if let Some(existing) = self.reports.iter_mut().find(|r| r.node_id == report.node_id) {
            *existing = report;
        } else {
            self.reports.push(report);
        }

        // 古いレポートを削除
        let cutoff = Instant::now() - Duration::from_secs(30);
        self.reports.retain(|r| r.timestamp > cutoff);
    }

    /// 最適化が必要か
    pub fn needs_optimize(&self) -> bool {
        self.last_optimize.elapsed() >= self.optimize_interval
    }

    /// スワームツリーを最適化し、必要なアクションを返す
    pub fn optimize(&mut self) -> Vec<SwarmAction> {
        self.last_optimize = Instant::now();
        let mut actions = Vec::new();

        if self.reports.len() < 2 {
            return actions;
        }

        // 1. 過負荷ノードの検出と再割当て
        let overloaded: Vec<u32> = self.reports.iter()
            .filter(|r| r.current_children >= r.relay_capacity && r.relay_capacity > 0)
            .map(|r| r.node_id)
            .collect();

        let underloaded: Vec<(u32, f64)> = self.reports.iter()
            .filter(|r| r.current_children < r.relay_capacity.saturating_sub(1))
            .map(|r| (r.node_id, r.relay_fitness()))
            .collect();

        for overloaded_id in &overloaded {
            // 最もフィットネスの高い空きノードに再割当て
            if let Some(&(best_parent, _)) = underloaded.iter()
                .filter(|(id, _)| id != overloaded_id)
                .max_by(|(_, a), (_, b)| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal))
            {
                actions.push(SwarmAction::Reassign {
                    node_id: *overloaded_id,
                    new_parent_id: best_parent,
                });
            }
        }

        // 2. 低品質ノードのビットレート調整
        for report in &self.reports {
            let target = BitrateLevel::from_loss_rate(report.loss_rate);
            // ノードのロス率に基づいてビットレート推奨
            if report.loss_rate > 5.0 {
                actions.push(SwarmAction::AdjustBitrate {
                    node_id: report.node_id,
                    level: target,
                });
            }
        }

        // 3. バッテリー低下ノードの降格
        for report in &self.reports {
            if report.battery < 15 && report.battery != 255 && report.relay_capacity > 0 {
                actions.push(SwarmAction::DemoteFromRelay {
                    node_id: report.node_id,
                });
            }
        }

        // 4. 高品質ノードのリレー昇格
        for report in &self.reports {
            if report.relay_capacity == 0
                && report.bandwidth_kbps > 5000
                && report.loss_rate < 1.0
                && (report.battery > 80 || report.battery == 255)
            {
                actions.push(SwarmAction::PromoteToRelay {
                    node_id: report.node_id,
                });
            }
        }

        actions
    }

    /// 全ノードの統計
    pub fn stats(&self) -> SwarmStats {
        let total = self.reports.len();
        let relays = self.reports.iter().filter(|r| r.relay_capacity > 0).count();
        let avg_loss = if total > 0 {
            self.reports.iter().map(|r| r.loss_rate).sum::<f32>() / total as f32
        } else { 0.0 };
        let avg_rtt = if total > 0 {
            self.reports.iter().map(|r| r.rtt_ms as f64).sum::<f64>() / total as f64
        } else { 0.0 };
        let avg_fitness = if !self.reports.is_empty() {
            self.reports.iter().map(|r| r.relay_fitness()).sum::<f64>() / total as f64
        } else { 0.0 };

        SwarmStats {
            total_nodes: total,
            relay_nodes: relays,
            leaf_nodes: total - relays,
            avg_loss_rate: avg_loss,
            avg_rtt_ms: avg_rtt,
            avg_fitness: avg_fitness,
        }
    }
}

impl Default for SwarmOptimizer {
    fn default() -> Self {
        Self::new()
    }
}

/// スワーム全体の統計
#[derive(Debug, Clone)]
pub struct SwarmStats {
    pub total_nodes: usize,
    pub relay_nodes: usize,
    pub leaf_nodes: usize,
    pub avg_loss_rate: f32,
    pub avg_rtt_ms: f64,
    pub avg_fitness: f64,
}

impl std::fmt::Display for SwarmStats {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f,
            "nodes={} (relay={}, leaf={}) loss={:.1}% rtt={:.0}ms fitness={:.1}",
            self.total_nodes, self.relay_nodes, self.leaf_nodes,
            self.avg_loss_rate, self.avg_rtt_ms, self.avg_fitness,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // ── Adaptive Bitrate Tests ──

    #[test]
    fn test_bitrate_from_loss() {
        assert_eq!(BitrateLevel::from_loss_rate(0.5), BitrateLevel::High);
        assert_eq!(BitrateLevel::from_loss_rate(3.0), BitrateLevel::Standard);
        assert_eq!(BitrateLevel::from_loss_rate(10.0), BitrateLevel::Low);
        assert_eq!(BitrateLevel::from_loss_rate(20.0), BitrateLevel::Ultra);
    }

    #[test]
    fn test_loss_tracker_no_loss() {
        let mut tracker = LossTracker::new();
        for _ in 0..100 {
            tracker.record_received();
        }
        assert!(tracker.loss_rate() < 0.01);
        assert_eq!(tracker.current_level(), BitrateLevel::Standard);
    }

    #[test]
    fn test_loss_tracker_high_loss() {
        let mut tracker = LossTracker::new();
        // Force last_change to be old enough for hysteresis
        tracker.last_change = Instant::now() - Duration::from_secs(10);
        for _ in 0..80 {
            tracker.record_received();
        }
        for _ in 0..20 {
            tracker.record_lost(1);
        }
        assert!(tracker.loss_rate() > 15.0);
        let new_level = tracker.update();
        assert!(new_level.is_some());
        assert_eq!(tracker.current_level(), BitrateLevel::Ultra);
    }

    #[test]
    fn test_fec_overhead() {
        assert_eq!(BitrateLevel::High.fec_overhead(), 0.0);
        assert!(BitrateLevel::Ultra.fec_overhead() > 0.0);
    }

    // ── Transport Selector Tests ──

    #[test]
    fn test_transport_priority() {
        assert!(TransportMode::LanMulticast.priority() < TransportMode::Quic.priority());
        assert!(TransportMode::Quic.priority() < TransportMode::WebSocket.priority());
    }

    #[test]
    fn test_transport_selection() {
        let mut selector = TransportSelector::new();

        selector.record_probe(ProbeResult {
            mode: TransportMode::Quic,
            success: true,
            latency_ms: Some(25),
            timestamp: Instant::now(),
        });
        selector.record_probe(ProbeResult {
            mode: TransportMode::LanMulticast,
            success: false,
            latency_ms: None,
            timestamp: Instant::now(),
        });

        let best = selector.select_best();
        assert_eq!(best, TransportMode::Quic);
    }

    #[test]
    fn test_transport_fallback() {
        let mut selector = TransportSelector::new();
        selector.current = TransportMode::Quic;

        assert_eq!(selector.fallback(), TransportMode::WebSocket);
        assert_eq!(selector.fallback(), TransportMode::HttpProgressive);
    }

    // ── Swarm Intelligence Tests ──

    #[test]
    fn test_node_fitness() {
        let good_node = NodeReport {
            node_id: 1,
            quality_score: 95,
            bandwidth_kbps: 10000,
            loss_rate: 0.5,
            rtt_ms: 10,
            relay_capacity: 10,
            current_children: 2,
            battery: 255, // AC power
            transport: TransportMode::Quic,
            timestamp: Instant::now(),
        };

        let bad_node = NodeReport {
            node_id: 2,
            quality_score: 20,
            bandwidth_kbps: 500,
            loss_rate: 15.0,
            rtt_ms: 200,
            relay_capacity: 2,
            current_children: 2,
            battery: 10,
            transport: TransportMode::WebSocket,
            timestamp: Instant::now(),
        };

        assert!(good_node.relay_fitness() > bad_node.relay_fitness());
    }

    #[test]
    fn test_swarm_optimizer_overload() {
        let mut optimizer = SwarmOptimizer::new();

        // Overloaded relay
        optimizer.update_node(NodeReport {
            node_id: 1,
            quality_score: 90,
            bandwidth_kbps: 10000,
            loss_rate: 1.0,
            rtt_ms: 20,
            relay_capacity: 3,
            current_children: 3, // at capacity
            battery: 255,
            transport: TransportMode::Quic,
            timestamp: Instant::now(),
        });

        // Underloaded relay
        optimizer.update_node(NodeReport {
            node_id: 2,
            quality_score: 85,
            bandwidth_kbps: 8000,
            loss_rate: 0.5,
            rtt_ms: 15,
            relay_capacity: 5,
            current_children: 1, // has room
            battery: 255,
            transport: TransportMode::Quic,
            timestamp: Instant::now(),
        });

        let actions = optimizer.optimize();
        let has_reassign = actions.iter().any(|a| matches!(a, SwarmAction::Reassign { .. }));
        assert!(has_reassign, "Should reassign from overloaded to underloaded");
    }

    #[test]
    fn test_swarm_low_battery_demotion() {
        let mut optimizer = SwarmOptimizer::new();

        optimizer.update_node(NodeReport {
            node_id: 1,
            quality_score: 50,
            bandwidth_kbps: 5000,
            loss_rate: 2.0,
            rtt_ms: 30,
            relay_capacity: 5,
            current_children: 2,
            battery: 10, // Low battery!
            transport: TransportMode::Quic,
            timestamp: Instant::now(),
        });

        // Need at least 2 nodes
        optimizer.update_node(NodeReport {
            node_id: 2,
            quality_score: 90,
            bandwidth_kbps: 10000,
            loss_rate: 0.5,
            rtt_ms: 10,
            relay_capacity: 10,
            current_children: 0,
            battery: 255,
            transport: TransportMode::Quic,
            timestamp: Instant::now(),
        });

        let actions = optimizer.optimize();
        let has_demote = actions.iter().any(|a| matches!(a, SwarmAction::DemoteFromRelay { node_id: 1 }));
        assert!(has_demote, "Should demote low-battery relay node");
    }

    #[test]
    fn test_swarm_promote_good_node() {
        let mut optimizer = SwarmOptimizer::new();

        // High quality leaf node (not yet a relay)
        optimizer.update_node(NodeReport {
            node_id: 1,
            quality_score: 95,
            bandwidth_kbps: 20000,
            loss_rate: 0.1,
            rtt_ms: 5,
            relay_capacity: 0, // Not a relay yet
            current_children: 0,
            battery: 255,
            transport: TransportMode::Quic,
            timestamp: Instant::now(),
        });

        optimizer.update_node(NodeReport {
            node_id: 2,
            quality_score: 50,
            bandwidth_kbps: 1000,
            loss_rate: 5.0,
            rtt_ms: 100,
            relay_capacity: 0,
            current_children: 0,
            battery: 50,
            transport: TransportMode::WebSocket,
            timestamp: Instant::now(),
        });

        let actions = optimizer.optimize();
        let has_promote = actions.iter().any(|a| matches!(a, SwarmAction::PromoteToRelay { node_id: 1 }));
        assert!(has_promote, "Should promote high-quality node to relay");
    }

    #[test]
    fn test_swarm_stats() {
        let mut optimizer = SwarmOptimizer::new();

        for i in 0..5 {
            optimizer.update_node(NodeReport {
                node_id: i,
                quality_score: 80,
                bandwidth_kbps: 5000,
                loss_rate: 2.0,
                rtt_ms: 20,
                relay_capacity: if i < 2 { 5 } else { 0 },
                current_children: 0,
                battery: 255,
                transport: TransportMode::Quic,
                timestamp: Instant::now(),
            });
        }

        let stats = optimizer.stats();
        assert_eq!(stats.total_nodes, 5);
        assert_eq!(stats.relay_nodes, 2);
        assert_eq!(stats.leaf_nodes, 3);
        assert!(stats.avg_loss_rate > 1.0);
    }
}
