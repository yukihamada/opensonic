//! Main entry point for the Soluna audio relay SDK.
//!
//! ```rust,no_run
//! use soluna_sdk::SolunaClient;
//!
//! let mut client = SolunaClient::new();
//! client.on_audio(|samples, channels| {
//!     // Process float32 PCM samples
//! });
//! client.connect("my-channel").unwrap();
//! // ...
//! client.disconnect();
//! ```

use std::io;
use std::sync::{Arc, Mutex};

use crate::parser::{constants, parse_ost_packet, OSTPacket};
use crate::player::decode_packet_to_f32;
use crate::relay::RelayConnection;

/// Connection state.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnectionState {
    Disconnected,
    Connected,
    Error,
}

type AudioCallback = Box<dyn Fn(&[f32], usize) + Send + 'static>;
type PacketCallback = Box<dyn Fn(&OSTPacket) + Send + 'static>;
type StateCallback = Box<dyn Fn(ConnectionState) + Send + 'static>;

/// Main Soluna client.
pub struct SolunaClient {
    connection: Option<RelayConnection>,
    channel: String,
    device_name: String,
    state: ConnectionState,
    on_audio_cb: Arc<Mutex<Option<AudioCallback>>>,
    on_packet_cb: Arc<Mutex<Option<PacketCallback>>>,
    on_state_cb: Arc<Mutex<Option<StateCallback>>>,
    #[cfg(feature = "audio")]
    audio_player: Option<crate::player::cpal_player::AudioPlayer>,
}

impl SolunaClient {
    pub fn new() -> Self {
        let device_name = std::env::var("HOSTNAME")
            .unwrap_or_else(|_| "SolunaSDK-Rust".to_string());

        Self {
            connection: None,
            channel: String::new(),
            device_name,
            state: ConnectionState::Disconnected,
            on_audio_cb: Arc::new(Mutex::new(None)),
            on_packet_cb: Arc::new(Mutex::new(None)),
            on_state_cb: Arc::new(Mutex::new(None)),
            #[cfg(feature = "audio")]
            audio_player: None,
        }
    }

    pub fn is_connected(&self) -> bool {
        self.state == ConnectionState::Connected
    }

    pub fn channel(&self) -> &str {
        &self.channel
    }

    /// Register a callback for decoded float32 PCM audio.
    pub fn on_audio<F: Fn(&[f32], usize) + Send + 'static>(&mut self, callback: F) {
        *self.on_audio_cb.lock().unwrap() = Some(Box::new(callback));
    }

    /// Register a callback for parsed OSTP packets.
    pub fn on_packet<F: Fn(&OSTPacket) + Send + 'static>(&mut self, callback: F) {
        *self.on_packet_cb.lock().unwrap() = Some(Box::new(callback));
    }

    /// Register a callback for state changes.
    pub fn on_state<F: Fn(ConnectionState) + Send + 'static>(&mut self, callback: F) {
        *self.on_state_cb.lock().unwrap() = Some(Box::new(callback));
    }

    /// Connect to the Soluna relay and start receiving audio.
    pub fn connect(&mut self, channel: &str) -> io::Result<()> {
        self.connect_with(channel, constants::DEFAULT_HOST, constants::DEFAULT_PORT)
    }

    /// Connect with custom host and port.
    pub fn connect_with(&mut self, channel: &str, host: &str, port: u16) -> io::Result<()> {
        if self.is_connected() {
            return Ok(());
        }

        self.channel = channel.to_string();

        #[cfg(feature = "audio")]
        {
            let mut player = crate::player::cpal_player::AudioPlayer::new();
            if let Err(e) = player.start() {
                eprintln!("[SolunaSDK] Audio player start error: {e}");
            }
            self.audio_player = Some(player);
        }

        let mut conn = RelayConnection::new(channel, host, port, &self.device_name);

        let audio_cb = self.on_audio_cb.clone();
        let packet_cb = self.on_packet_cb.clone();
        #[cfg(feature = "audio")]
        let audio_player_buf = self
            .audio_player
            .as_ref()
            .map(|_| Arc::new(Mutex::new(Vec::<f32>::new())));

        conn.connect(
            move |data| {
                if let Some(packet) = parse_ost_packet(data) {
                    // Notify packet callback
                    if let Ok(guard) = packet_cb.lock() {
                        if let Some(cb) = guard.as_ref() {
                            cb(&packet);
                        }
                    }

                    // Decode and notify audio callback
                    if let Some(samples) = decode_packet_to_f32(&packet) {
                        if let Ok(guard) = audio_cb.lock() {
                            if let Some(cb) = guard.as_ref() {
                                cb(&samples, packet.channels);
                            }
                        }
                    }
                }
            },
            |_msg| {
                // Control messages ignored
            },
        )?;

        self.connection = Some(conn);
        self.state = ConnectionState::Connected;
        self.notify_state(ConnectionState::Connected);
        Ok(())
    }

    /// Disconnect from the relay.
    pub fn disconnect(&mut self) {
        if let Some(mut conn) = self.connection.take() {
            conn.disconnect();
        }

        #[cfg(feature = "audio")]
        {
            if let Some(mut player) = self.audio_player.take() {
                player.stop();
            }
        }

        self.state = ConnectionState::Disconnected;
        self.channel.clear();
        self.notify_state(ConnectionState::Disconnected);
    }

    /// Switch to a different channel.
    pub fn set_channel(&mut self, name: &str) -> io::Result<()> {
        let was_connected = self.is_connected();
        if was_connected {
            self.disconnect();
        }
        if was_connected {
            self.connect(name)?;
        }
        Ok(())
    }

    fn notify_state(&self, state: ConnectionState) {
        if let Ok(guard) = self.on_state_cb.lock() {
            if let Some(cb) = guard.as_ref() {
                cb(state);
            }
        }
    }
}

impl Default for SolunaClient {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for SolunaClient {
    fn drop(&mut self) {
        self.disconnect();
    }
}
