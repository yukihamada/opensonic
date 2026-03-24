//! WebTransport server — HTTP/3 + QUIC datagrams for browser audio streaming.
//!
//! Accepts WebTransport sessions at `/wt?channel=<name>` and bridges them
//! to the existing UDP relay server, same pattern as the QUIC bridge.
//!
//! Audio packets flow as QUIC unreliable datagrams (no HoL blocking).
//! Control messages (JOIN, META, etc.) use reliable QUIC streams.

use std::net::SocketAddr;
use std::sync::Arc;

use tokio::net::UdpSocket;
use tokio::sync::{mpsc, RwLock};
use wtransport::tls::Identity;
use wtransport::{Endpoint, ServerConfig};

use crate::BridgeState;

/// Start the WebTransport server alongside the existing QUIC bridge.
pub async fn run_webtransport_server(
    wt_port: u16,
    relay_addr: SocketAddr,
    state: Arc<RwLock<BridgeState>>,
    cert_path: &str,
    key_path: &str,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let identity = Identity::load_pemfiles(cert_path, key_path).await?;

    let config = ServerConfig::builder()
        .with_bind_address(format!("0.0.0.0:{wt_port}").parse()?)
        .with_identity(identity)
        .build();

    let endpoint = Endpoint::server(config)?;

    eprintln!("[webtransport] listening on :{wt_port}, forwarding to {relay_addr}");

    loop {
        let incoming = endpoint.accept().await;
        let relay_addr = relay_addr;
        let state = state.clone();

        tokio::spawn(async move {
            if let Err(e) = handle_wt_session(incoming, relay_addr, state).await {
                eprintln!("[webtransport] session error: {e}");
            }
        });
    }
}

/// Handle a single WebTransport session.
async fn handle_wt_session(
    incoming: wtransport::endpoint::IncomingSession,
    relay_addr: SocketAddr,
    state: Arc<RwLock<BridgeState>>,
) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let session_request = incoming.await?;

    // Extract channel from URL query: /wt?channel=<name>
    let path = session_request.path().to_string();
    let channel = extract_channel(&path).unwrap_or_default();

    eprintln!("[webtransport] session request: path={path}, channel={channel}");

    let session = session_request.accept().await?;
    let remote = session.remote_address();

    eprintln!("[webtransport] session accepted from {remote}");

    // Create a UDP socket to talk to the local relay
    let udp = Arc::new(UdpSocket::bind("0.0.0.0:0").await?);
    let (shutdown_tx, mut shutdown_rx) = mpsc::channel::<()>(1);

    // Register connection for mining
    let conn_id = {
        let mut s = state.write().await;
        let id = s.next_id;
        s.next_id += 1;
        s.mining.register(id, remote.to_string());
        if !channel.is_empty() {
            if let Some(stats) = s.mining.get_stats(id) {
                stats.set_channel(&channel);
            }
        }
        id
    };

    // Send JOIN to relay if channel is specified
    if !channel.is_empty() {
        let join_msg = format!("JOIN:{channel}:\n");
        let _ = udp.send_to(join_msg.as_bytes(), relay_addr).await;
    }

    let session = Arc::new(session);

    // Task 1: WebTransport datagrams (browser -> relay) — audio packets
    let session_dg = session.clone();
    let udp_dg = udp.clone();
    let state_dg = state.clone();
    let dg_rx_task = tokio::spawn(async move {
        loop {
            match session_dg.receive_datagram().await {
                Ok(datagram) => {
                    let data: &[u8] = &datagram;
                    let len = data.len() as u64;
                    let _ = udp_dg.send_to(data, relay_addr).await;
                    let s = state_dg.read().await;
                    if let Some(stats) = s.mining.get_stats(conn_id) {
                        stats.record_audio(len);
                    }
                }
                Err(_) => break,
            }
        }
    });

    // Task 2: WebTransport bidirectional streams (browser -> relay) — control
    let session_bi = session.clone();
    let udp_bi = udp.clone();
    let state_bi = state.clone();
    let bi_task = tokio::spawn(async move {
        loop {
            match session_bi.accept_bi().await {
                Ok((_send, mut recv)) => {
                    let mut buf = vec![0u8; 4096];
                    match recv.read(&mut buf).await {
                        Ok(Some(n)) => {
                            let data = &buf[..n];
                            let len = n as u64;
                            // Extract channel from JOIN messages
                            if data.starts_with(b"JOIN:") {
                                if let Ok(msg) = std::str::from_utf8(data) {
                                    let ch = msg.trim_start_matches("JOIN:")
                                        .split(':').next()
                                        .unwrap_or("")
                                        .trim();
                                    let s = state_bi.read().await;
                                    if let Some(stats) = s.mining.get_stats(conn_id) {
                                        stats.set_channel(ch);
                                    }
                                }
                            }
                            let _ = udp_bi.send_to(data, relay_addr).await;
                            let s = state_bi.read().await;
                            if let Some(stats) = s.mining.get_stats(conn_id) {
                                stats.record_control(len);
                            }
                        }
                        _ => continue,
                    }
                }
                Err(_) => break,
            }
        }
    });

    // Task 2b: Also accept unidirectional streams for control messages
    let session_uni = session.clone();
    let udp_uni = udp.clone();
    let state_uni = state.clone();
    let uni_task = tokio::spawn(async move {
        loop {
            match session_uni.accept_uni().await {
                Ok(mut recv) => {
                    let mut buf = vec![0u8; 4096];
                    match recv.read(&mut buf).await {
                        Ok(Some(n)) => {
                            let data = &buf[..n];
                            let len = n as u64;
                            let _ = udp_uni.send_to(data, relay_addr).await;
                            let s = state_uni.read().await;
                            if let Some(stats) = s.mining.get_stats(conn_id) {
                                stats.record_control(len);
                            }
                        }
                        _ => continue,
                    }
                }
                Err(_) => break,
            }
        }
    });

    // Task 3: Relay UDP responses -> WebTransport (datagrams for audio, streams for control)
    let session_tx = session.clone();
    let udp_rx = udp.clone();
    let rx_task = tokio::spawn(async move {
        let mut buf = vec![0u8; 8192];
        loop {
            tokio::select! {
                result = udp_rx.recv_from(&mut buf) => {
                    match result {
                        Ok((n, _)) => {
                            let data = &buf[..n];
                            // RTP/OSTP audio packets (version bits = 0x80) -> datagrams
                            if n >= 2 && (data[0] & 0xC0) == 0x80 {
                                // Skip packets too large for QUIC datagram (~1200B MTU)
                                if n > 1200 {
                                    continue; // FLAC/large PCM — skip, don't break session
                                }
                                if let Err(e) = session_tx.send_datagram(data) {
                                    // Non-fatal: skip this packet, don't close session
                                    eprintln!("[webtransport] datagram skip: {e}, size={n}");
                                    continue;
                                }
                            } else {
                                // Control responses -> reliable unidirectional stream
                                match session_tx.open_uni().await {
                                    Ok(opening) => {
                                        match opening.await {
                                            Ok(mut send) => {
                                                let _ = send.write_all(data).await;
                                                let _ = send.finish().await;
                                            }
                                            Err(_) => {}
                                        }
                                    }
                                    Err(_) => {}
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

    // Wait for session to close
    let close_reason = session.closed().await;
    eprintln!("[webtransport] session from {remote} closed: {close_reason:?}");

    // Cleanup
    dg_rx_task.abort();
    bi_task.abort();
    uni_task.abort();
    rx_task.abort();
    let _ = shutdown_tx.send(()).await;

    {
        let mut s = state.write().await;
        if let Some(reward) = s.mining.unregister(conn_id) {
            eprintln!("[mining/wt] connection {}: {:.1}MB relayed, {:.6} ENAI earned (channel: {})",
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

/// Extract `channel` query parameter from a path like `/wt?channel=foo`
fn extract_channel(path: &str) -> Option<String> {
    let query = path.split('?').nth(1)?;
    for param in query.split('&') {
        if let Some(value) = param.strip_prefix("channel=") {
            return Some(urlencoding_decode(value));
        }
    }
    None
}

/// Minimal URL decoding (percent-encoded -> plain text)
fn urlencoding_decode(s: &str) -> String {
    let mut result = String::with_capacity(s.len());
    let mut chars = s.bytes();
    while let Some(b) = chars.next() {
        if b == b'%' {
            let hi = chars.next().and_then(hex_val);
            let lo = chars.next().and_then(hex_val);
            if let (Some(h), Some(l)) = (hi, lo) {
                result.push((h << 4 | l) as char);
            }
        } else if b == b'+' {
            result.push(' ');
        } else {
            result.push(b as char);
        }
    }
    result
}

fn hex_val(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_extract_channel() {
        assert_eq!(extract_channel("/wt?channel=jazz"), Some("jazz".to_string()));
        assert_eq!(extract_channel("/wt?foo=bar&channel=lofi"), Some("lofi".to_string()));
        assert_eq!(extract_channel("/wt"), None);
        assert_eq!(extract_channel("/wt?channel=my%20channel"), Some("my channel".to_string()));
    }
}
