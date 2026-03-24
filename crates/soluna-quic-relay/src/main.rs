//! soluna-quic-bridge — QUIC front-end for the existing UDP relay server.
//!
//! Runs alongside `soluna-relay` (C++) and bridges QUIC clients to it:
//!
//! ```text
//! QUIC Client     ←→ [soluna-quic-bridge :5101] ←UDP→ [soluna-relay :5100]
//! WebTransport    ←→ [soluna-quic-bridge :4443] ←UDP→ [soluna-relay :5100]
//! ```
//!
//! Features:
//! - QUIC datagrams (unreliable) → forwarded as UDP to relay (audio)
//! - QUIC streams (reliable) → control messages forwarded as UDP to relay
//! - **WebTransport** (HTTP/3 + QUIC datagrams) for browser clients
//! - UDP responses from relay → forwarded back as QUIC datagrams/streams
//! - **Relay Mining**: per-connection traffic metering for ENAI token rewards
//!
//! This avoids modifying the 10K-line C++ relay while giving clients
//! all QUIC benefits: TLS 1.3, connection migration, NAT traversal.

mod mining;
mod webtransport;

use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::Arc;

use mining::{MiningManager, RelayTier};
use quinn::{Connection, Endpoint};
use soluna_core::quic::{generate_self_signed_cert, make_server_config};
use tokio::net::UdpSocket;
use tokio::sync::{mpsc, RwLock};

/// Default ports
const QUIC_LISTEN_PORT: u16 = 5101;
const UDP_RELAY_PORT: u16 = 5100;
const WT_LISTEN_PORT: u16 = 4443;

/// Bridge state shared across all connections (QUIC + WebTransport).
pub(crate) struct BridgeState {
    pub connections: HashMap<usize, ConnState>,
    pub next_id: usize,
    pub mining: MiningManager,
}

pub(crate) struct ConnState {
    pub udp: Arc<UdpSocket>,
    pub shutdown_tx: mpsc::Sender<()>,
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();

    let quic_port = std::env::var("QUIC_PORT")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(QUIC_LISTEN_PORT);

    let relay_addr: SocketAddr = std::env::var("RELAY_ADDR")
        .unwrap_or_else(|_| format!("127.0.0.1:{UDP_RELAY_PORT}"))
        .parse()?;

    let wallet = std::env::var("SOLANA_WALLET")
        .unwrap_or_else(|_| "unset".to_string());

    let tier = match std::env::var("RELAY_TIER").as_deref() {
        Ok("origin") => RelayTier::Origin,
        Ok("region") => RelayTier::Region,
        Ok("swarm") => RelayTier::Swarm,
        _ => RelayTier::Edge,
    };

    // Parse --cert and --key CLI flags for WebTransport TLS
    let cert_path = get_arg(&args, "--cert")
        .or_else(|| std::env::var("WT_CERT").ok());
    let key_path = get_arg(&args, "--key")
        .or_else(|| std::env::var("WT_KEY").ok());
    let wt_port: u16 = get_arg(&args, "--wt-port")
        .and_then(|s| s.parse().ok())
        .or_else(|| std::env::var("WT_PORT").ok().and_then(|s| s.parse().ok()))
        .unwrap_or(WT_LISTEN_PORT);

    // Generate self-signed cert for QUIC bridge
    let (certs, key) = generate_self_signed_cert()?;
    let server_config = make_server_config(certs, key)?;

    let endpoint = Endpoint::server(
        server_config,
        format!("0.0.0.0:{quic_port}").parse()?,
    )?;

    eprintln!("[quic-bridge] listening on :{quic_port}, forwarding to {relay_addr}");
    eprintln!("[quic-bridge] mining: tier={tier:?}, wallet={}", &wallet[..8.min(wallet.len())]);

    let state = Arc::new(RwLock::new(BridgeState {
        connections: HashMap::new(),
        next_id: 0,
        mining: MiningManager::new(tier, wallet),
    }));

    // Periodic mining stats logger
    let state_stats = state.clone();
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(std::time::Duration::from_secs(60));
        loop {
            interval.tick().await;
            let s = state_stats.read().await;
            let summary = s.mining.summary();
            eprintln!("{summary}");
        }
    });

    // Start WebTransport server if TLS certificate is provided
    if let (Some(cert), Some(key)) = (cert_path, key_path) {
        let state_wt = state.clone();
        eprintln!("[webtransport] starting with cert={cert}, key={key}");
        tokio::spawn(async move {
            if let Err(e) = webtransport::run_webtransport_server(
                wt_port, relay_addr, state_wt, &cert, &key,
            ).await {
                eprintln!("[webtransport] fatal error: {e}");
            }
        });
    } else {
        eprintln!("[webtransport] disabled (no --cert/--key provided)");
        eprintln!("[webtransport] usage: --cert /path/to/fullchain.pem --key /path/to/privkey.pem");
    }

    // Accept QUIC connections (existing behavior)
    while let Some(incoming) = endpoint.accept().await {
        let relay_addr = relay_addr;
        let state = state.clone();

        tokio::spawn(async move {
            match incoming.await {
                Ok(conn) => {
                    if let Err(e) = handle_connection(conn, relay_addr, state).await {
                        eprintln!("[quic-bridge] connection error: {e}");
                    }
                }
                Err(e) => eprintln!("[quic-bridge] accept error: {e}"),
            }
        });
    }

    Ok(())
}

/// Parse a CLI flag value: --flag value
fn get_arg(args: &[String], flag: &str) -> Option<String> {
    args.iter().position(|a| a == flag)
        .and_then(|i| args.get(i + 1))
        .cloned()
}

async fn handle_connection(
    conn: Connection,
    relay_addr: SocketAddr,
    state: Arc<RwLock<BridgeState>>,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let remote = conn.remote_address();
    eprintln!("[quic-bridge] new connection from {remote}");

    let udp = Arc::new(UdpSocket::bind("0.0.0.0:0").await?);
    let (shutdown_tx, mut shutdown_rx) = mpsc::channel::<()>(1);

    let conn_id = {
        let mut s = state.write().await;
        let id = s.next_id;
        s.next_id += 1;
        s.mining.register(id, remote.to_string());
        s.connections.insert(id, ConnState {
            udp: udp.clone(),
            shutdown_tx: shutdown_tx.clone(),
        });
        id
    };

    // Task 1: QUIC datagrams → UDP relay (audio)
    let conn_dg = conn.clone();
    let udp_dg = udp.clone();
    let state_dg = state.clone();
    let dg_task = tokio::spawn(async move {
        loop {
            match conn_dg.read_datagram().await {
                Ok(data) => {
                    let len = data.len() as u64;
                    let _ = udp_dg.send_to(&data, relay_addr).await;
                    // Record mining stats
                    let s = state_dg.read().await;
                    if let Some(stats) = s.mining.get_stats(conn_id) {
                        stats.record_audio(len);
                    }
                }
                Err(_) => break,
            }
        }
    });

    // Task 2: QUIC uni streams (control messages) → UDP relay
    let conn_stream = conn.clone();
    let udp_stream = udp.clone();
    let state_stream = state.clone();
    let stream_task = tokio::spawn(async move {
        loop {
            match conn_stream.accept_uni().await {
                Ok(mut recv) => {
                    match recv.read_to_end(4096).await {
                        Ok(data) => {
                            let len = data.len() as u64;
                            // Extract channel name from JOIN messages
                            if data.starts_with(b"JOIN:") {
                                if let Ok(msg) = std::str::from_utf8(&data) {
                                    let channel = msg.trim_start_matches("JOIN:")
                                        .split(':').next()
                                        .unwrap_or("")
                                        .trim();
                                    let s = state_stream.read().await;
                                    if let Some(stats) = s.mining.get_stats(conn_id) {
                                        stats.set_channel(channel);
                                    }
                                }
                            }
                            let _ = udp_stream.send_to(&data, relay_addr).await;
                            let s = state_stream.read().await;
                            if let Some(stats) = s.mining.get_stats(conn_id) {
                                stats.record_control(len);
                            }
                        }
                        Err(_) => continue,
                    }
                }
                Err(_) => break,
            }
        }
    });

    // Task 3: UDP relay responses → QUIC
    let conn_rx = conn.clone();
    let udp_rx = udp.clone();
    let rx_task = tokio::spawn(async move {
        let mut buf = vec![0u8; 2048];
        loop {
            tokio::select! {
                result = udp_rx.recv_from(&mut buf) => {
                    match result {
                        Ok((n, _)) => {
                            let data = &buf[..n];
                            if n >= 2 && (data[0] & 0xC0) == 0x80 {
                                let _ = conn_rx.send_datagram(data.to_vec().into());
                            } else {
                                if let Ok(mut send) = conn_rx.open_uni().await {
                                    let _ = send.write_all(data).await;
                                    let _ = send.finish();
                                }
                            }
                        }
                        Err(_) => break,
                    }
                }
                _ = shutdown_rx.recv() => break,
            }
        }
    });

    // Wait for connection to close
    let closed = conn.closed().await;
    eprintln!("[quic-bridge] connection from {remote} closed: {closed}");

    // Cleanup + mining reward calculation
    dg_task.abort();
    stream_task.abort();
    rx_task.abort();
    let _ = shutdown_tx.send(()).await;

    {
        let mut s = state.write().await;
        if let Some(reward) = s.mining.unregister(conn_id) {
            eprintln!("[mining] connection {}: {:.1}MB relayed, {:.6} ENAI earned (channel: {})",
                reward.conn_id,
                reward.audio_bytes as f64 / 1_048_576.0,
                reward.reward_enai,
                reward.channel,
            );
        }
        s.connections.remove(&conn_id);
    }

    Ok(())
}
