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
import { type RelayConnectionState } from "./RelayConnection.js";
import { type OSTPacket } from "./OSTPacketParser.js";
type EventMap = {
    audio: [Float32Array, number];
    state: [RelayConnectionState];
    packet: [OSTPacket];
};
type EventHandler<K extends keyof EventMap> = (...args: EventMap[K]) => void;
export declare class SolunaClient {
    private connection;
    private audioPlayer;
    private handlers;
    private _state;
    private _channel;
    private _playbackEnabled;
    /** Current connection state. */
    get state(): RelayConnectionState;
    /** Current channel name. */
    get channel(): string;
    /** Whether built-in audio playback is enabled. Default: true. */
    get playbackEnabled(): boolean;
    set playbackEnabled(v: boolean);
    /**
     * Register an event handler.
     * - 'audio': (samples: Float32Array, channels: number) => void
     * - 'state': (state: RelayConnectionState) => void
     * - 'packet': (packet: OSTPacket) => void
     */
    on<K extends keyof EventMap>(event: K, handler: EventHandler<K>): this;
    /** Remove an event handler. */
    off<K extends keyof EventMap>(event: K, handler: EventHandler<K>): this;
    private emit;
    /**
     * Connect to the Soluna relay and start receiving audio.
     * @param channel - Channel name to join.
     * @param host - Relay host (default: relay.solun.art).
     * @param tls - Use TLS (default: true).
     */
    connect(channel: string, host?: string, tls?: boolean): void;
    /** Disconnect from the relay and stop playback. */
    disconnect(): void;
    /** Switch to a different channel. */
    setChannel(name: string): void;
    private handlePacket;
}
export {};
