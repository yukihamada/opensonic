//! Full System Test — 音声品質・帯域適応・QUIC・PoL・検証を一気通貫テスト
//!
//! テスト内容:
//! 1. 音声生成 → OSTP パケット化 → 送信 → 受信 → デコード → 品質検証
//! 2. パケットロスシミュレーション → Adaptive Bitrate 動作確認
//! 3. QUIC datagram ラウンドトリップ
//! 4. PoL ハッシュチェーン + Audio Verification 統合
//! 5. コスト試算

use soluna_core::ostp;
use soluna_core::easy;
use soluna_core::pol;
use soluna_core::adaptive;
use soluna_core::verify;
use soluna_core::ring_buffer::RingBuffer;

use std::f32::consts::TAU;

// ── Helper: generate sine wave ──
fn generate_sine(freq: f32, sample_rate: u32, duration_secs: f32) -> Vec<f32> {
    let num_samples = (sample_rate as f32 * duration_secs) as usize;
    (0..num_samples)
        .map(|i| (i as f32 * freq / sample_rate as f32 * TAU).sin() * 0.8)
        .collect()
}

// ── Helper: build OSTP packet from f32 samples ──
fn build_ostp_packet(seq: u16, timestamp: u32, ssrc: u32, samples: &[f32]) -> Vec<u8> {
    let mut packet = Vec::with_capacity(24 + samples.len() * 4 + 4);
    // RTP Header (V=2, X=1)
    packet.push(0x90);
    packet.push(ostp::PT_F32);
    packet.extend_from_slice(&seq.to_be_bytes());
    packet.extend_from_slice(&timestamp.to_be_bytes());
    packet.extend_from_slice(&ssrc.to_be_bytes());
    // RTP Extension Header
    packet.extend_from_slice(&0x4F53u16.to_be_bytes()); // "OS"
    packet.extend_from_slice(&2u16.to_be_bytes());
    // OSTP Header
    packet.extend_from_slice(&0u16.to_be_bytes()); // stream_id
    packet.extend_from_slice(&0u16.to_be_bytes()); // seq_ext
    packet.extend_from_slice(&timestamp.to_be_bytes()); // media_ts
    // Payload
    let payload_start = packet.len();
    for &s in samples {
        packet.extend_from_slice(&s.to_le_bytes());
    }
    // CRC-32 trailer (over payload only, big-endian)
    let crc = ostp::crc32_ieee(&packet[payload_start..]);
    packet.extend_from_slice(&crc.to_be_bytes());
    packet
}

// ══════════════════════════════════════════════════════════
// Test 1: 音声品質 — 生成→パケット化→パース→復元→比較
// ══════════════════════════════════════════════════════════
#[test]
fn test_audio_quality_perfect_transmission() {
    println!("\n═══ Test 1: Audio Quality (Perfect Transmission) ═══");

    let sample_rate = 48000;
    let freq = 440.0; // A4
    let duration = 1.0; // 1 second
    let samples_per_packet = 240; // 5ms
    let ssrc = 0x12345678;

    // Generate clean sine wave
    let original = generate_sine(freq, sample_rate, duration);
    println!("  Generated: {:.0}Hz sine, {}Hz, {:.1}s, {} samples",
        freq, sample_rate, duration, original.len());

    // Packetize
    let mut packets = Vec::new();
    for (i, chunk) in original.chunks(samples_per_packet).enumerate() {
        let seq = i as u16;
        let ts = i as u32 * samples_per_packet as u32;
        packets.push(build_ostp_packet(seq, ts, ssrc, chunk));
    }
    println!("  Packetized: {} OSTP packets ({} bytes each)",
        packets.len(), packets.first().map(|p| p.len()).unwrap_or(0));

    // Parse and reconstruct
    let mut reconstructed = Vec::new();
    let mut seq_verifier = verify::SequenceVerifier::new();

    for packet in &packets {
        let parsed = ostp::parse(packet).expect("Failed to parse OSTP packet");
        assert_eq!(parsed.rtp.payload_type, ostp::PT_F32);
        assert_eq!(parsed.rtp.ssrc, ssrc);

        seq_verifier.verify_packet(parsed.rtp.sequence);

        // Decode f32 payload
        let samples: Vec<f32> = parsed.payload.chunks_exact(4)
            .map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect();
        reconstructed.extend_from_slice(&samples);
    }

    let seq_stats = seq_verifier.stats();
    println!("  Sequence: {}", seq_stats);
    assert_eq!(seq_stats.lost, 0);
    assert_eq!(seq_stats.duplicates, 0);

    // Compare original vs reconstructed
    let len = original.len().min(reconstructed.len());
    let mut max_error: f32 = 0.0;
    let mut sum_sq_error: f64 = 0.0;
    for i in 0..len {
        let err = (original[i] - reconstructed[i]).abs();
        max_error = max_error.max(err);
        sum_sq_error += (err as f64) * (err as f64);
    }
    let rms_error = (sum_sq_error / len as f64).sqrt();

    println!("  Reconstruction: {} / {} samples", len, original.len());
    println!("  Max Error: {:.10} (should be 0.0 for lossless)", max_error);
    println!("  RMS Error: {:.10}", rms_error);

    assert_eq!(max_error, 0.0, "Lossless transmission should have zero error");
    assert_eq!(reconstructed[..len], original[..len]);

    // Fingerprint verification
    let fp_orig = verify::AudioFingerprint::from_samples(&original, 480);
    let fp_recv = verify::AudioFingerprint::from_samples(&reconstructed, 480);
    let similarity = fp_orig.similarity(&fp_recv);
    println!("  Fingerprint Similarity: {:.1}%", similarity * 100.0);
    assert_eq!(similarity, 1.0);

    println!("  ✓ PASSED — Perfect audio quality, zero error\n");
}

// ══════════════════════════════════════════════════════════
// Test 2: 音声品質 — パケットロスあり
// ══════════════════════════════════════════════════════════
#[test]
fn test_audio_quality_with_loss() {
    println!("\n═══ Test 2: Audio Quality (5% Packet Loss) ═══");

    let sample_rate = 48000;
    let original = generate_sine(440.0, sample_rate, 2.0);
    let samples_per_packet = 240;
    let ssrc = 0xAABBCCDD;

    let mut packets = Vec::new();
    for (i, chunk) in original.chunks(samples_per_packet).enumerate() {
        packets.push((i as u16, build_ostp_packet(i as u16, i as u32 * 240, ssrc, chunk)));
    }

    // Simulate 5% loss
    let mut received = Vec::new();
    let mut seq_verifier = verify::SequenceVerifier::new();
    let ring = RingBuffer::new(48000, 1); // 1 sec buffer

    let mut lost_count = 0;
    for (seq, packet) in &packets {
        if *seq % 20 == 7 { // Drop every 20th packet (5%)
            lost_count += 1;
            continue;
        }

        let parsed = ostp::parse(packet).unwrap();
        seq_verifier.verify_packet(parsed.rtp.sequence);

        let samples: Vec<f32> = parsed.payload.chunks_exact(4)
            .map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect();
        ring.write(&samples);
        received.extend_from_slice(&samples);
    }

    let stats = seq_verifier.stats();
    println!("  Total packets: {}, Lost: {}, Received: {}",
        packets.len(), lost_count, packets.len() - lost_count);
    println!("  Sequence stats: {}", stats);
    assert!(stats.loss_rate > 4.0 && stats.loss_rate < 6.0,
        "Expected ~5% loss, got {:.1}%", stats.loss_rate);

    // Fingerprint comparison (should still be high due to energy-based matching)
    let fp_orig = verify::AudioFingerprint::from_samples(&original, 480);
    let fp_recv = verify::AudioFingerprint::from_samples(&received, 480);
    let similarity = fp_orig.similarity(&fp_recv);
    println!("  Fingerprint Similarity: {:.1}%", similarity * 100.0);
    assert!(similarity > 0.4, "With 5% loss, energy-based similarity should be reasonable");

    println!("  ✓ PASSED — Audio degraded gracefully under 5% loss\n");
}

// ══════════════════════════════════════════════════════════
// Test 3: Ring Buffer — リアルタイム読み書き
// ══════════════════════════════════════════════════════════
#[test]
fn test_ring_buffer_realtime() {
    println!("\n═══ Test 3: Ring Buffer Real-time Read/Write ═══");

    let rb = RingBuffer::new(4800, 1); // 100ms @ 48kHz

    // Simulate: writer thread produces 240 samples/5ms
    let audio = generate_sine(440.0, 48000, 0.1); // 100ms

    // Write in chunks
    for chunk in audio.chunks(240) {
        rb.write(chunk);
    }
    println!("  Written: {} samples", audio.len());
    println!("  Available: {} samples", rb.available());
    assert_eq!(rb.available(), audio.len());

    // Read back
    let mut output = vec![0.0f32; audio.len()];
    let n = rb.read(&mut output);
    assert_eq!(n, audio.len());
    assert_eq!(output, audio);

    println!("  Read back: {} samples, exact match ✓", n);
    println!("  ✓ PASSED — Lock-free ring buffer works correctly\n");
}

// ══════════════════════════════════════════════════════════
// Test 4: Adaptive Bitrate — ロス率変動への追従
// ══════════════════════════════════════════════════════════
#[test]
fn test_adaptive_bitrate() {
    println!("\n═══ Test 4: Adaptive Bitrate ═══");

    let mut tracker = adaptive::LossTracker::new();
    // Backdate to allow immediate changes
    tracker.last_change = std::time::Instant::now() - std::time::Duration::from_secs(100);

    // Phase 1: No loss → High quality
    for _ in 0..200 {
        tracker.record_received();
    }
    tracker.update();
    println!("  Phase 1 (0% loss): {:?} @ {}kbps",
        tracker.current_level(), tracker.current_level().bitrate() / 1000);
    assert_eq!(tracker.current_level(), adaptive::BitrateLevel::High);

    // Phase 2: 10% loss → should downgrade from High
    tracker.last_change = std::time::Instant::now() - std::time::Duration::from_secs(100);
    for _ in 0..90 { tracker.record_received(); }
    for _ in 0..10 { tracker.record_lost(1); }
    tracker.update();
    println!("  Phase 2 (10% loss): {:?} @ {}kbps",
        tracker.current_level(), tracker.current_level().bitrate() / 1000);
    assert!(tracker.current_level() < adaptive::BitrateLevel::High,
        "Should downgrade from High under 10% loss");

    // Phase 3: Heavy loss → should downgrade further
    tracker.last_change = std::time::Instant::now() - std::time::Duration::from_secs(100);
    for _ in 0..75 { tracker.record_received(); }
    for _ in 0..25 { tracker.record_lost(1); }
    tracker.update();
    println!("  Phase 3 (heavy loss): {:?} @ {}kbps, FEC overhead: {:.0}%",
        tracker.current_level(), tracker.current_level().bitrate() / 1000,
        tracker.current_level().fec_overhead() * 100.0);
    assert!(tracker.current_level() <= adaptive::BitrateLevel::Standard,
        "Should be at most Standard under heavy loss");

    println!("  ✓ PASSED — Bitrate adapts to network conditions\n");
}

// ══════════════════════════════════════════════════════════
// Test 5: Transport Selection — フォールバックチェーン
// ══════════════════════════════════════════════════════════
#[test]
fn test_transport_selection() {
    println!("\n═══ Test 5: Transport Selection ═══");

    let mut selector = adaptive::TransportSelector::new();

    // Probe results
    selector.record_probe(adaptive::ProbeResult {
        mode: adaptive::TransportMode::LanMulticast,
        success: false, latency_ms: None,
        timestamp: std::time::Instant::now(),
    });
    println!("  LAN Multicast: FAIL (not on LAN)");

    selector.record_probe(adaptive::ProbeResult {
        mode: adaptive::TransportMode::P2pDirect,
        success: false, latency_ms: None,
        timestamp: std::time::Instant::now(),
    });
    println!("  P2P Direct: FAIL (symmetric NAT)");

    selector.record_probe(adaptive::ProbeResult {
        mode: adaptive::TransportMode::Quic,
        success: true, latency_ms: Some(28),
        timestamp: std::time::Instant::now(),
    });
    println!("  QUIC: OK (28ms)");

    selector.record_probe(adaptive::ProbeResult {
        mode: adaptive::TransportMode::WebSocket,
        success: true, latency_ms: Some(85),
        timestamp: std::time::Instant::now(),
    });
    println!("  WebSocket: OK (85ms)");

    let best = selector.select_best();
    println!("  Selected: {:?} (latency: {}ms, encrypted: {})",
        best, best.expected_latency_ms(), best.is_encrypted());
    assert_eq!(best, adaptive::TransportMode::Quic);

    // Simulate QUIC failure → fallback
    let fallback = selector.fallback();
    println!("  Fallback: {:?}", fallback);
    assert_eq!(fallback, adaptive::TransportMode::WebSocket);

    println!("  ✓ PASSED — Transport fallback chain works\n");
}

// ══════════════════════════════════════════════════════════
// Test 6: Swarm Intelligence — ノード最適化
// ══════════════════════════════════════════════════════════
#[test]
fn test_swarm_intelligence() {
    println!("\n═══ Test 6: Swarm Intelligence ═══");

    let mut optimizer = adaptive::SwarmOptimizer::new();

    // Add diverse nodes
    let nodes = vec![
        ("Tokyo-Fiber", 0xA1, 50000, 0.2, 5, 10, 3, 255u8),
        ("Osaka-4G", 0xA2, 3000, 8.0, 45, 2, 2, 60),
        ("Berlin-WiFi", 0xA3, 15000, 1.5, 120, 5, 1, 255),
        ("Mobile-User", 0xA4, 1000, 12.0, 80, 0, 0, 25),
        ("Koe-Device", 0xA5, 8000, 0.5, 10, 8, 0, 255),
    ];

    for (name, id, bw, loss, rtt, cap, children, bat) in &nodes {
        let report = adaptive::NodeReport {
            node_id: *id as u32,
            quality_score: 80,
            bandwidth_kbps: *bw,
            loss_rate: *loss,
            rtt_ms: *rtt,
            relay_capacity: *cap,
            current_children: *children,
            battery: *bat,
            transport: adaptive::TransportMode::Quic,
            timestamp: std::time::Instant::now(),
        };
        let fitness = report.relay_fitness();
        println!("  {} (id=0x{:02X}): bw={}kbps loss={:.1}% rtt={}ms cap={}/{} bat={} → fitness={:.1}",
            name, id, bw, loss, rtt, children, cap, bat, fitness);
        optimizer.update_node(report);
    }

    let actions = optimizer.optimize();
    println!("\n  Optimization actions:");
    for action in &actions {
        match action {
            adaptive::SwarmAction::Reassign { node_id, new_parent_id } =>
                println!("    → Reassign 0x{:02X} to parent 0x{:02X}", node_id, new_parent_id),
            adaptive::SwarmAction::PromoteToRelay { node_id } =>
                println!("    → Promote 0x{:02X} to relay", node_id),
            adaptive::SwarmAction::DemoteFromRelay { node_id } =>
                println!("    → Demote 0x{:02X} from relay (low battery)", node_id),
            adaptive::SwarmAction::AdjustBitrate { node_id, level } =>
                println!("    → Adjust 0x{:02X} to {:?} ({}kbps)", node_id, level, level.bitrate()/1000),
            adaptive::SwarmAction::NoChange => println!("    → No change needed"),
        }
    }

    let stats = optimizer.stats();
    println!("\n  Swarm: {}", stats);

    assert!(!actions.is_empty(), "Optimizer should produce actions for this network");
    // Verify optimizer produces meaningful actions (bitrate adjustments for lossy nodes)
    let has_bitrate_adjust = actions.iter().any(|a| matches!(a, adaptive::SwarmAction::AdjustBitrate { .. }));
    assert!(has_bitrate_adjust, "Should adjust bitrate for high-loss nodes");

    println!("  ✓ PASSED — Swarm intelligence optimizes network\n");
}

// ══════════════════════════════════════════════════════════
// Test 7: PoL + Audio Verification 統合
// ══════════════════════════════════════════════════════════
#[test]
fn test_pol_with_audio_verification() {
    println!("\n═══ Test 7: PoL + Audio Verification ═══");

    let sample_rate = 48000;
    let ssrc = 0xDEADBEEF;
    let channel = "soluna";

    // Generate 2 seconds of music-like audio (chord)
    let duration = 2.0;
    let a4 = generate_sine(440.0, sample_rate, duration);
    let e5 = generate_sine(659.25, sample_rate, duration);
    let cs5 = generate_sine(554.37, sample_rate, duration);
    let audio: Vec<f32> = a4.iter().zip(e5.iter()).zip(cs5.iter())
        .map(|((a, e), c)| (a + e + c) / 3.0)
        .collect();

    println!("  Audio: A major chord (A4+C#5+E5), {:.1}s, {} samples", duration, audio.len());

    // Sender side
    let mut sender_verifier = verify::AudioVerifier::new(sample_rate, 1);
    let mut sender_pol = pol::HashChain::with_channel(channel);

    // Receiver side
    let mut receiver_verifier = verify::AudioVerifier::new(sample_rate, 1);
    let mut receiver_pol = pol::HashChain::with_channel(channel);

    let samples_per_packet = 240;
    let mut packets_sent = 0u16;
    let mut packets_received = 0u16;

    for (i, chunk) in audio.chunks(samples_per_packet).enumerate() {
        let seq = i as u16;
        let ts = i as u32 * samples_per_packet as u32;
        let packet = build_ostp_packet(seq, ts, ssrc, chunk);
        packets_sent += 1;

        // Sender records
        sender_verifier.record_samples(chunk);
        let crc = crc32_simple(&packet[24..]); // payload CRC
        sender_pol.append(pol::ListenRecord {
            seq, timestamp: ts, ssrc, payload_crc: crc,
            received_at: 1710000000000 + i as u64 * 5,
        });

        // Simulate transmission (2% loss)
        if seq % 50 == 13 {
            continue; // lost
        }

        // Receiver processes
        let parsed = ostp::parse(&packet).unwrap();
        receiver_verifier.verify_packet(parsed.rtp.sequence);
        let samples: Vec<f32> = parsed.payload.chunks_exact(4)
            .map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect();
        receiver_verifier.record_samples(&samples);
        receiver_pol.append(pol::ListenRecord {
            seq, timestamp: ts, ssrc, payload_crc: crc,
            received_at: 1710000000000 + i as u64 * 5 + 15, // +15ms latency
        });
        packets_received += 1;
    }

    println!("  Sent: {}, Received: {}, Lost: {}",
        packets_sent, packets_received, packets_sent - packets_received);

    // Audio verification
    let report = receiver_verifier.compare_with(&sender_verifier);
    println!("  Audio Verification: {}", report);

    // PoL comparison
    let sender_snap = sender_pol.snapshot();
    let receiver_snap = receiver_pol.snapshot();
    println!("  Sender PoL:   root={}, count={}",
        pol::hash_to_hex(&sender_snap.merkle_root)[..16].to_string(), sender_snap.record_count);
    println!("  Receiver PoL: root={}, count={}",
        pol::hash_to_hex(&receiver_snap.merkle_root)[..16].to_string(), receiver_snap.record_count);

    // PoL roots should differ (different packet sets due to loss)
    // but both are valid proofs of their respective receptions
    assert_ne!(sender_snap.merkle_root, receiver_snap.merkle_root,
        "Roots should differ due to packet loss");
    assert!(sender_snap.record_count > receiver_snap.record_count);

    println!("  ✓ PASSED — PoL + Audio Verification integrated correctly\n");
}

// ══════════════════════════════════════════════════════════
// Test 8: QUIC Datagram ラウンドトリップ
// ══════════════════════════════════════════════════════════
#[cfg(feature = "quic")]
#[tokio::test]
async fn test_quic_audio_roundtrip() {
    use soluna_core::quic;

    println!("\n═══ Test 8: QUIC Audio Round-trip ═══");

    let (certs, key) = quic::generate_self_signed_cert().unwrap();
    let server_config = quic::make_server_config(certs, key).unwrap();

    let server = quinn::Endpoint::server(server_config, "127.0.0.1:0".parse().unwrap()).unwrap();
    let addr = server.local_addr().unwrap();

    // Server: echo audio datagrams
    let server_task = tokio::spawn(async move {
        let incoming = server.accept().await.unwrap();
        let conn = incoming.await.unwrap();
        let mut count = 0;
        loop {
            match tokio::time::timeout(std::time::Duration::from_millis(500), conn.read_datagram()).await {
                Ok(Ok(data)) => {
                    conn.send_datagram(data).unwrap();
                    count += 1;
                }
                _ => break,
            }
        }
        count
    });

    // Client
    let mut transport = quic::QuicTransportBuilder::new(addr)
        .skip_cert_verify(true)
        .connect()
        .await
        .unwrap();

    // Send 100 OSTP audio packets
    let audio = generate_sine(440.0, 48000, 0.5); // 0.5s
    let mut sent = 0;
    for (i, chunk) in audio.chunks(240).enumerate() {
        let packet = build_ostp_packet(i as u16, i as u32 * 240, 0xBEEF, chunk);
        transport.send_audio(&packet).unwrap();
        sent += 1;
    }

    // Receive echoes
    let mut received = 0;
    for _ in 0..sent {
        match tokio::time::timeout(std::time::Duration::from_millis(200), transport.recv_audio()).await {
            Ok(Some(data)) => {
                assert!(data.len() >= 24);
                let parsed = ostp::parse(&data).unwrap();
                assert_eq!(parsed.rtp.ssrc, 0xBEEF);
                received += 1;
            }
            _ => break,
        }
    }

    transport.close();
    let server_count = server_task.await.unwrap();

    println!("  Sent: {} OSTP packets via QUIC datagram", sent);
    println!("  Server echoed: {}", server_count);
    println!("  Client received: {}", received);
    assert!(received > sent * 90 / 100, "Should receive >90% of echoed packets");

    println!("  ✓ PASSED — QUIC audio round-trip works\n");
}

// ══════════════════════════════════════════════════════════
// Test 9: コスト試算
// ══════════════════════════════════════════════════════════
#[test]
fn test_cost_analysis() {
    println!("\n═══ Test 9: Cost Analysis ═══");
    println!();

    struct Scenario {
        name: &'static str,
        listeners: u64,
        hours_per_day: f64,
        bitrate_kbps: u32,
        days: u32,
    }

    let scenarios = [
        Scenario { name: "個人DJ (週末)", listeners: 50, hours_per_day: 4.0, bitrate_kbps: 128, days: 8 },
        Scenario { name: "お店BGM (毎日)", listeners: 20, hours_per_day: 10.0, bitrate_kbps: 128, days: 30 },
        Scenario { name: "中規模イベント", listeners: 1000, hours_per_day: 6.0, bitrate_kbps: 256, days: 3 },
        Scenario { name: "大規模フェス", listeners: 100_000, hours_per_day: 12.0, bitrate_kbps: 128, days: 3 },
    ];

    let sol_price_usd = 150.0;
    let sol_per_tx = 0.000005;
    let pol_submissions_per_hour = 1;

    println!("  {:<24} {:>10} {:>12} {:>12} {:>12} {:>10}",
        "Scenario", "Listeners", "Bandwidth", "PoL Cost", "Mining Rev", "Net/day");
    println!("  {}", "─".repeat(84));

    for s in &scenarios {
        let bytes_per_sec = s.bitrate_kbps as f64 * 1000.0 / 8.0;
        let bytes_per_hour = bytes_per_sec * 3600.0;
        let daily_bandwidth_gb = bytes_per_hour * s.hours_per_day * s.listeners as f64 / 1e9;

        // PoL cost: each listener submits once per hour
        let daily_pol_txs = s.listeners * pol_submissions_per_hour as u64 * s.hours_per_day as u64;
        let daily_pol_sol = daily_pol_txs as f64 * sol_per_tx;
        let daily_pol_usd = daily_pol_sol * sol_price_usd;

        // Mining revenue: relay nodes earn from forwarding
        let daily_bytes = bytes_per_hour * s.hours_per_day * s.listeners as f64;
        let daily_enai = daily_bytes * 1e-9 * 0.25; // Edge tier
        // Assume 1 ENAI ≈ $0.01 (placeholder)
        let daily_mining_usd = daily_enai * 0.01;

        let net_per_day = daily_mining_usd - daily_pol_usd;

        println!("  {:<24} {:>10} {:>9.1} GB {:>8.4} USD {:>8.4} USD {:>+9.4} USD",
            s.name, s.listeners, daily_bandwidth_gb,
            daily_pol_usd, daily_mining_usd, net_per_day);
    }

    println!();
    println!("  Assumptions:");
    println!("    SOL price: ${:.0}", sol_price_usd);
    println!("    Tx fee: {} SOL (${:.4})", sol_per_tx, sol_per_tx * sol_price_usd);
    println!("    PoL submission: 1x/hour/listener");
    println!("    Mining tier: Edge (0.25x)");
    println!("    ENAI price: $0.01 (placeholder)");
    println!();

    // Solana program storage costs
    println!("  On-chain Storage Costs:");
    let rent_per_byte_year = 0.00000348; // SOL per byte per year (approx)
    let accts = [
        ("SolunaState", 89),
        ("ListenProof (per listener×channel)", 153),
        ("RelayNode (per operator)", 58),
        ("TrackRegistry (per track)", 89),
    ];
    for (name, size) in &accts {
        let rent_sol = *size as f64 * rent_per_byte_year;
        println!("    {:<40} {} bytes → {:.6} SOL/year (${:.4})",
            name, size, rent_sol, rent_sol * sol_price_usd);
    }

    println!();
    println!("  Program Deploy: ~0.7 SOL (${:.0}) — one-time", 0.7 * sol_price_usd);
    println!();
    println!("  ✓ Cost analysis complete\n");
}

// ── Simple CRC-32 ──
fn crc32_simple(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xEDB8_8320 & (0u32.wrapping_sub(crc & 1)));
        }
    }
    crc ^ 0xFFFF_FFFF
}
