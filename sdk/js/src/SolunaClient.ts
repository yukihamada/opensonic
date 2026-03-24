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

import { RelayConnection, type RelayConnectionState } from "./RelayConnection.js";
import { parseOSTPacket, type OSTPacket } from "./OSTPacketParser.js";
import { AudioPlayer } from "./AudioPlayer.js";

type EventMap = {
  audio: [Float32Array, number]; // [samples, channels]
  state: [RelayConnectionState];
  packet: [OSTPacket];
};

type EventHandler<K extends keyof EventMap> = (...args: EventMap[K]) => void;

export class SolunaClient {
  private connection: RelayConnection | null = null;
  private audioPlayer = new AudioPlayer();
  private handlers: { [K in keyof EventMap]?: Set<EventHandler<K>> } = {};
  private _state: RelayConnectionState = "disconnected";
  private _channel = "";
  private _playbackEnabled = true;

  /** Current connection state. */
  get state(): RelayConnectionState {
    return this._state;
  }

  /** Current channel name. */
  get channel(): string {
    return this._channel;
  }

  /** Whether built-in audio playback is enabled. Default: true. */
  get playbackEnabled(): boolean {
    return this._playbackEnabled;
  }
  set playbackEnabled(v: boolean) {
    this._playbackEnabled = v;
  }

  /**
   * Register an event handler.
   * - 'audio': (samples: Float32Array, channels: number) => void
   * - 'state': (state: RelayConnectionState) => void
   * - 'packet': (packet: OSTPacket) => void
   */
  on<K extends keyof EventMap>(event: K, handler: EventHandler<K>): this {
    if (!this.handlers[event]) {
      (this.handlers as any)[event] = new Set();
    }
    this.handlers[event]!.add(handler);
    return this;
  }

  /** Remove an event handler. */
  off<K extends keyof EventMap>(event: K, handler: EventHandler<K>): this {
    this.handlers[event]?.delete(handler);
    return this;
  }

  private emit<K extends keyof EventMap>(event: K, ...args: EventMap[K]): void {
    const set = this.handlers[event];
    if (set) {
      for (const handler of set) {
        (handler as any)(...args);
      }
    }
  }

  /**
   * Connect to the Soluna relay and start receiving audio.
   * @param channel - Channel name to join.
   * @param host - Relay host (default: relay.solun.art).
   * @param tls - Use TLS (default: true).
   */
  connect(channel: string, host?: string, tls?: boolean): void {
    if (this._state === "connected" || this._state === "connecting") return;

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
  disconnect(): void {
    this.connection?.disconnect();
    this.connection = null;
    this.audioPlayer.stop();
    this._state = "disconnected";
    this._channel = "";
    this.emit("state", "disconnected");
  }

  /** Switch to a different channel. */
  setChannel(name: string): void {
    const wasConnected = this._state === "connected";
    if (wasConnected) this.disconnect();
    if (wasConnected) this.connect(name);
  }

  private handlePacket(data: Uint8Array): void {
    const packet = parseOSTPacket(data);
    if (!packet) return;

    this.emit("packet", packet);

    if (this._playbackEnabled) {
      this.audioPlayer.playPacket(packet);
    }
  }
}
