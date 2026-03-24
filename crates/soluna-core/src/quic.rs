//! QUIC transport for WAN relay connections.
//!
//! Uses QUIC Unreliable Datagrams (RFC 9221) for audio and
//! reliable streams for control messages (JOIN, HELLO, META, etc.).
//!
//! # Architecture
//! ```text
//! QUIC Connection (TLS 1.3)
//! ├── Datagram (unreliable) → OSTP/RTP audio packets
//! └── Stream 0 (reliable)   → Control messages (text-based, same as UDP protocol)
//! ```

use std::net::SocketAddr;
use std::sync::Arc;

use quinn::{Connection, Endpoint, ClientConfig, TransportConfig};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use tokio::sync::mpsc;

/// Maximum datagram size for audio packets.
/// QUIC datagrams should fit in a single UDP packet to avoid fragmentation.
const MAX_DATAGRAM_SIZE: usize = 1200;

/// QUIC transport error.
#[derive(Debug)]
pub enum QuicError {
    Connect(quinn::ConnectionError),
    Write(quinn::WriteError),
    Io(std::io::Error),
    Tls(String),
    DatagramUnsupported,
}

impl std::fmt::Display for QuicError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            QuicError::Connect(e) => write!(f, "QUIC connect: {e}"),
            QuicError::Write(e) => write!(f, "QUIC write: {e}"),
            QuicError::Io(e) => write!(f, "IO: {e}"),
            QuicError::Tls(e) => write!(f, "TLS: {e}"),
            QuicError::DatagramUnsupported => write!(f, "server does not support QUIC datagrams"),
        }
    }
}

impl std::error::Error for QuicError {}

impl From<quinn::ConnectionError> for QuicError {
    fn from(e: quinn::ConnectionError) -> Self { QuicError::Connect(e) }
}

impl From<quinn::WriteError> for QuicError {
    fn from(e: quinn::WriteError) -> Self { QuicError::Write(e) }
}

impl From<std::io::Error> for QuicError {
    fn from(e: std::io::Error) -> Self { QuicError::Io(e) }
}

/// A QUIC connection to a Soluna relay server.
///
/// Audio packets are sent/received via unreliable datagrams.
/// Control messages use a reliable bidirectional stream.
pub struct QuicTransport {
    /// The underlying QUIC connection. Public for use by easy::RelayNode.
    pub connection: Connection,
    /// Channel for incoming control messages from the relay.
    control_rx: mpsc::UnboundedReceiver<Vec<u8>>,
    /// Channel for incoming audio datagrams from the relay.
    audio_rx: mpsc::UnboundedReceiver<Vec<u8>>,
}

/// Builder for configuring and establishing a QUIC connection.
pub struct QuicTransportBuilder {
    relay_addr: SocketAddr,
    server_name: String,
    skip_cert_verify: bool,
}

impl QuicTransportBuilder {
    /// Create a new builder targeting the given relay address.
    pub fn new(relay_addr: SocketAddr) -> Self {
        Self {
            relay_addr,
            server_name: "soluna-relay".to_string(),
            skip_cert_verify: true, // Self-signed certs by default
        }
    }

    /// Set the server name for TLS verification.
    pub fn server_name(mut self, name: &str) -> Self {
        self.server_name = name.to_string();
        self
    }

    /// Whether to skip TLS certificate verification (for self-signed certs).
    pub fn skip_cert_verify(mut self, skip: bool) -> Self {
        self.skip_cert_verify = skip;
        self
    }

    /// Connect to the relay and return a QuicTransport.
    pub async fn connect(self) -> Result<QuicTransport, QuicError> {
        let client_config = make_client_config(self.skip_cert_verify)?;
        let mut endpoint = Endpoint::client("0.0.0.0:0".parse().unwrap())?;
        endpoint.set_default_client_config(client_config);

        let connection = endpoint
            .connect(self.relay_addr, &self.server_name)
            .map_err(|e| QuicError::Io(std::io::Error::new(std::io::ErrorKind::Other, e)))?
            .await?;

        // Verify server supports datagrams
        if connection.max_datagram_size().is_none() {
            return Err(QuicError::DatagramUnsupported);
        }

        let (control_tx, control_rx) = mpsc::unbounded_channel();
        let (audio_tx, audio_rx) = mpsc::unbounded_channel();

        // Spawn receiver tasks
        let conn_clone = connection.clone();
        tokio::spawn(async move {
            datagram_recv_loop(conn_clone, audio_tx).await;
        });

        let conn_clone = connection.clone();
        tokio::spawn(async move {
            stream_recv_loop(conn_clone, control_tx).await;
        });

        Ok(QuicTransport {
            connection,
            control_rx,
            audio_rx,
        })
    }
}

impl QuicTransport {
    /// Send an OSTP/RTP audio packet as an unreliable datagram.
    ///
    /// If the packet is too large for a single datagram, it is silently dropped.
    /// This matches UDP semantics — lost packets are acceptable for real-time audio.
    #[inline]
    pub fn send_audio(&self, packet: &[u8]) -> Result<(), QuicError> {
        if packet.len() > MAX_DATAGRAM_SIZE {
            return Ok(()); // Too large, drop silently (same as UDP)
        }
        self.connection
            .send_datagram(packet.to_vec().into())
            .map_err(|e| QuicError::Io(std::io::Error::new(std::io::ErrorKind::Other, e)))
    }

    /// Send a control message (JOIN, HELLO, META, etc.) via a reliable stream.
    pub async fn send_control(&self, msg: &[u8]) -> Result<(), QuicError> {
        let mut send = self.connection.open_uni().await?;
        send.write_all(msg).await?;
        let _ = send.finish();
        Ok(())
    }

    /// Send a control message and wait for a response on the same stream.
    pub async fn send_control_bidi(&self, msg: &[u8]) -> Result<Vec<u8>, QuicError> {
        let (mut send, mut recv) = self.connection.open_bi().await?;
        send.write_all(msg).await?;
        let _ = send.finish();
        let response = recv.read_to_end(64 * 1024).await
            .map_err(|e| QuicError::Io(std::io::Error::new(std::io::ErrorKind::Other, e)))?;
        Ok(response)
    }

    /// Receive the next audio datagram. Returns None if the connection is closed.
    pub async fn recv_audio(&mut self) -> Option<Vec<u8>> {
        self.audio_rx.recv().await
    }

    /// Receive the next control message. Returns None if the connection is closed.
    pub async fn recv_control(&mut self) -> Option<Vec<u8>> {
        self.control_rx.recv().await
    }

    /// Try to receive an audio datagram without waiting.
    pub fn try_recv_audio(&mut self) -> Option<Vec<u8>> {
        self.audio_rx.try_recv().ok()
    }

    /// Check if the connection is still alive.
    pub fn is_connected(&self) -> bool {
        self.connection.close_reason().is_none()
    }

    /// Get the remote address.
    pub fn remote_addr(&self) -> SocketAddr {
        self.connection.remote_address()
    }

    /// Close the connection gracefully.
    pub fn close(&self) {
        self.connection.close(0u32.into(), b"bye");
    }
}

// ── Internal helpers ──

/// Receive datagrams and forward to the audio channel.
async fn datagram_recv_loop(conn: Connection, tx: mpsc::UnboundedSender<Vec<u8>>) {
    loop {
        match conn.read_datagram().await {
            Ok(bytes) => {
                if tx.send(bytes.to_vec()).is_err() {
                    break; // Receiver dropped
                }
            }
            Err(_) => break, // Connection closed
        }
    }
}

/// Accept incoming uni streams (control responses from relay) and forward.
async fn stream_recv_loop(conn: Connection, tx: mpsc::UnboundedSender<Vec<u8>>) {
    loop {
        match conn.accept_uni().await {
            Ok(mut recv) => {
                match recv.read_to_end(64 * 1024).await {
                    Ok(data) => {
                        if tx.send(data).is_err() {
                            break;
                        }
                    }
                    Err(_) => continue,
                }
            }
            Err(_) => break,
        }
    }
}

/// Create a self-signed TLS certificate for QUIC.
pub fn generate_self_signed_cert() -> Result<(Vec<CertificateDer<'static>>, PrivateKeyDer<'static>), QuicError> {
    let cert = rcgen::generate_simple_self_signed(vec!["soluna-relay".to_string()])
        .map_err(|e| QuicError::Tls(e.to_string()))?;
    let cert_der = CertificateDer::from(cert.cert);
    let key_der = PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(cert.key_pair.serialize_der()));
    Ok((vec![cert_der], key_der))
}

/// Build QUIC client config.
fn make_client_config(skip_cert_verify: bool) -> Result<ClientConfig, QuicError> {
    // Install default crypto provider
    let _ = rustls::crypto::ring::default_provider().install_default();

    let crypto = if skip_cert_verify {
        rustls::ClientConfig::builder()
            .dangerous()
            .with_custom_certificate_verifier(Arc::new(SkipServerVerification))
            .with_no_client_auth()
    } else {
        // For production: use platform root certs
        let mut root_store = rustls::RootCertStore::empty();
        root_store.extend(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
        rustls::ClientConfig::builder()
            .with_root_certificates(root_store)
            .with_no_client_auth()
    };

    let mut transport = TransportConfig::default();
    // Enable unreliable datagrams (RFC 9221)
    transport.datagram_receive_buffer_size(Some(2 * 1024 * 1024)); // 2MB receive buffer
    transport.max_idle_timeout(Some(
        quinn::IdleTimeout::try_from(std::time::Duration::from_secs(30)).unwrap(),
    ));
    // Keep-alive to prevent NAT timeout
    transport.keep_alive_interval(Some(std::time::Duration::from_secs(5)));

    let mut config = ClientConfig::new(Arc::new(
        quinn::crypto::rustls::QuicClientConfig::try_from(crypto)
            .map_err(|e| QuicError::Tls(format!("{e}")))?
    ));
    config.transport_config(Arc::new(transport));

    Ok(config)
}

/// Build QUIC server config (for relay).
pub fn make_server_config(
    certs: Vec<CertificateDer<'static>>,
    key: PrivateKeyDer<'static>,
) -> Result<quinn::ServerConfig, QuicError> {
    // Install default crypto provider
    let _ = rustls::crypto::ring::default_provider().install_default();

    let crypto = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(certs, key)
        .map_err(|e| QuicError::Tls(format!("{e}")))?;

    let mut transport = TransportConfig::default();
    transport.datagram_receive_buffer_size(Some(2 * 1024 * 1024));
    transport.max_idle_timeout(Some(
        quinn::IdleTimeout::try_from(std::time::Duration::from_secs(30)).unwrap(),
    ));

    let mut config = quinn::ServerConfig::with_crypto(Arc::new(
        quinn::crypto::rustls::QuicServerConfig::try_from(crypto)
            .map_err(|e| QuicError::Tls(format!("{e}")))?
    ));
    config.transport_config(Arc::new(transport));

    Ok(config)
}

/// TLS certificate verifier that accepts any certificate (for self-signed relay certs).
#[derive(Debug)]
struct SkipServerVerification;

impl rustls::client::danger::ServerCertVerifier for SkipServerVerification {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &rustls::pki_types::ServerName<'_>,
        _ocsp_response: &[u8],
        _now: rustls::pki_types::UnixTime,
    ) -> Result<rustls::client::danger::ServerCertVerified, rustls::Error> {
        Ok(rustls::client::danger::ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }

    fn verify_tls13_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        rustls::crypto::ring::default_provider()
            .signature_verification_algorithms
            .supported_schemes()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_generate_cert() {
        let (certs, _key) = generate_self_signed_cert().unwrap();
        assert_eq!(certs.len(), 1);
    }

    #[tokio::test]
    async fn test_client_server_datagram() {
        // Generate certs
        let (certs, key) = generate_self_signed_cert().unwrap();
        let server_config = make_server_config(certs, key).unwrap();

        // Start server
        let server_endpoint = Endpoint::server(
            server_config,
            "127.0.0.1:0".parse().unwrap(),
        ).unwrap();
        let server_addr = server_endpoint.local_addr().unwrap();

        // Server task: accept one connection, echo datagrams
        let server = tokio::spawn(async move {
            let incoming = server_endpoint.accept().await.unwrap();
            let conn = incoming.await.unwrap();

            // Read one datagram
            let data = conn.read_datagram().await.unwrap();
            // Echo it back
            conn.send_datagram(data).unwrap();

            // Keep alive briefly
            tokio::time::sleep(std::time::Duration::from_millis(100)).await;
            conn.close(0u32.into(), b"done");
        });

        // Client connects
        let mut transport = QuicTransportBuilder::new(server_addr)
            .skip_cert_verify(true)
            .connect()
            .await
            .unwrap();

        assert!(transport.is_connected());

        // Send audio datagram
        let test_packet = vec![0x80, 0x62, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                               0x12, 0x34, 0x56, 0x78, 0x4F, 0x53, 0x00, 0x02,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0xDE, 0xAD, 0xBE, 0xEF];
        transport.send_audio(&test_packet).unwrap();

        // Receive echo
        let echoed = transport.recv_audio().await.unwrap();
        assert_eq!(echoed, test_packet);

        transport.close();
        let _ = server.await;
    }
}
