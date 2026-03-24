//! Low-level UDP socket connection to the Soluna relay server.
//! Uses std::net::UdpSocket, matching the Swift/C++ implementation.

use std::io;
use std::net::{ToSocketAddrs, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use crate::parser::constants;

/// UDP socket connection to the Soluna relay.
pub struct RelayConnection {
    channel: String,
    host: String,
    port: u16,
    device_name: String,
    socket: Option<UdpSocket>,
    running: Arc<AtomicBool>,
    recv_handle: Option<thread::JoinHandle<()>>,
    heartbeat_handle: Option<thread::JoinHandle<()>>,
}

impl RelayConnection {
    pub fn new(channel: &str, host: &str, port: u16, device_name: &str) -> Self {
        Self {
            channel: channel.to_string(),
            host: host.to_string(),
            port,
            device_name: device_name.to_string(),
            socket: None,
            running: Arc::new(AtomicBool::new(false)),
            recv_handle: None,
            heartbeat_handle: None,
        }
    }

    /// Open the UDP socket, send HELLO/JOIN, and start the receive loop.
    /// Returns an error if DNS resolution or socket creation fails.
    ///
    /// `on_packet` is called on the recv thread when an audio packet arrives.
    /// `on_control` is called on the recv thread for text control messages.
    pub fn connect<F, G>(
        &mut self,
        on_packet: F,
        on_control: G,
    ) -> io::Result<()>
    where
        F: Fn(&[u8]) + Send + 'static,
        G: Fn(&str) + Send + 'static,
    {
        if self.running.load(Ordering::SeqCst) {
            return Ok(());
        }

        // DNS resolve
        let addr_str = format!("{}:{}", self.host, self.port);
        let addr = addr_str
            .to_socket_addrs()?
            .find(|a| a.is_ipv4())
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "DNS resolution failed"))?;

        // Create UDP socket
        let socket = UdpSocket::bind("0.0.0.0:0")?;
        socket.set_read_timeout(Some(Duration::from_secs(1)))?;
        socket.connect(addr)?;

        // Send HELLO x3 (100ms apart)
        for i in 0..3 {
            let _ = socket.send(b"HELLO\n");
            if i < 2 {
                thread::sleep(Duration::from_millis(100));
            }
        }

        // JOIN
        let join_msg = format!("JOIN:{}::{}\n", self.channel, self.device_name);
        let _ = socket.send(join_msg.as_bytes());

        self.running.store(true, Ordering::SeqCst);

        // Clone for threads
        let recv_socket = socket.try_clone()?;
        let running_recv = self.running.clone();

        // Receive thread
        self.recv_handle = Some(thread::spawn(move || {
            let mut buf = vec![0u8; constants::RECV_BUFFER_SIZE];
            while running_recv.load(Ordering::SeqCst) {
                match recv_socket.recv(&mut buf) {
                    Ok(n) if n >= constants::RTP_HEADER_SIZE => {
                        if (buf[0] & 0xC0) == 0x80 {
                            on_packet(&buf[..n]);
                        } else if let Ok(msg) = std::str::from_utf8(&buf[..n]) {
                            on_control(msg);
                        }
                    }
                    _ => continue,
                }
            }
        }));

        // Heartbeat thread
        let heartbeat_socket = socket.try_clone()?;
        let running_hb = self.running.clone();
        let channel = self.channel.clone();
        let device_name = self.device_name.clone();

        self.heartbeat_handle = Some(thread::spawn(move || {
            while running_hb.load(Ordering::SeqCst) {
                thread::sleep(Duration::from_secs(constants::HEARTBEAT_INTERVAL_SECS));
                if !running_hb.load(Ordering::SeqCst) {
                    break;
                }
                let _ = heartbeat_socket.send(b"HELLO\n");
                let join_msg = format!("JOIN:{}::{}\n", channel, device_name);
                let _ = heartbeat_socket.send(join_msg.as_bytes());
            }
        }));

        self.socket = Some(socket);
        Ok(())
    }

    /// Close the connection.
    pub fn disconnect(&mut self) {
        self.running.store(false, Ordering::SeqCst);

        // Drop the socket to unblock recv
        self.socket = None;

        if let Some(h) = self.recv_handle.take() {
            let _ = h.join();
        }
        if let Some(h) = self.heartbeat_handle.take() {
            let _ = h.join();
        }
    }

    pub fn is_connected(&self) -> bool {
        self.running.load(Ordering::SeqCst)
    }
}

impl Drop for RelayConnection {
    fn drop(&mut self) {
        self.disconnect();
    }
}
