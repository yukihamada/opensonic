/**
 * Main entry point for the Soluna audio relay SDK (browser).
 *
 * Connects to the Soluna relay server via WebSocket, receives OSTP/RTP
 * audio packets, decodes them, and plays them through the Web Audio API.
 *
 * Usage:
 *   import { SolunaClient } from '@soluna/sdk'
 *   const client = new SolunaClient()
 *   client.on('audio', (samples) => { ... })
 *   client.connect('my-channel')
 */
import { RelayConnection } from "./RelayConnection.js";
import { parseOSTPacket } from "./OSTPacketParser.js";
import { AudioPlayer } from "./AudioPlayer.js";
export class SolunaClient {
    connection = null;
    audioPlayer = new AudioPlayer();
    handlers = {};
    _state = "disconnected";
    _channel = "";
    _playbackEnabled = true;
    /** Current connection state. */
    get state() {
        return this._state;
    }
    /** Current channel name. */
    get channel() {
        return this._channel;
    }
    /** Whether built-in audio playback is enabled. Default: true. */
    get playbackEnabled() {
        return this._playbackEnabled;
    }
    set playbackEnabled(v) {
        this._playbackEnabled = v;
    }
    /**
     * Register an event handler.
     * - 'audio': (samples: Float32Array, channels: number) => void
     * - 'state': (state: RelayConnectionState) => void
     * - 'packet': (packet: OSTPacket) => void
     */
    on(event, handler) {
        if (!this.handlers[event]) {
            this.handlers[event] = new Set();
        }
        this.handlers[event].add(handler);
        return this;
    }
    /** Remove an event handler. */
    off(event, handler) {
        this.handlers[event]?.delete(handler);
        return this;
    }
    emit(event, ...args) {
        const set = this.handlers[event];
        if (set) {
            for (const handler of set) {
                handler(...args);
            }
        }
    }
    /**
     * Connect to the Soluna relay and start receiving audio.
     * @param channel - Channel name to join.
     * @param host - Relay host (default: relay.solun.art).
     * @param tls - Use TLS (default: true).
     */
    connect(channel, host, tls) {
        if (this._state === "connected" || this._state === "connecting")
            return;
        this._channel = channel;
        if (this._playbackEnabled) {
            this.audioPlayer.start();
        }
        this.connection = new RelayConnection({
            channel,
            host,
            tls,
        });
        this.connection.onPacket = (data) => {
            this.handlePacket(data);
        };
        this.connection.onStateChange = (state) => {
            this._state = state;
            this.emit("state", state);
        };
        this.connection.connect();
    }
    /** Disconnect from the relay and stop playback. */
    disconnect() {
        this.connection?.disconnect();
        this.connection = null;
        this.audioPlayer.stop();
        this._state = "disconnected";
        this._channel = "";
        this.emit("state", "disconnected");
    }
    /** Switch to a different channel. */
    setChannel(name) {
        const wasConnected = this._state === "connected";
        if (wasConnected)
            this.disconnect();
        if (wasConnected)
            this.connect(name);
    }
    handlePacket(data) {
        const packet = parseOSTPacket(data);
        if (!packet)
            return;
        this.emit("packet", packet);
        if (this._playbackEnabled) {
            this.audioPlayer.playPacket(packet);
        }
    }
}
