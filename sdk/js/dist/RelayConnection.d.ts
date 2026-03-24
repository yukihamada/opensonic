/**
 * WebSocket connection to the Soluna relay server.
 *
 * Browsers cannot use raw UDP sockets, so we connect via the relay's
 * WebSocket endpoint: wss://relay.solun.art/ws/audio?channel=NAME
 * Binary frames contain raw RTP/OSTP packets.
 */
export type RelayConnectionState = "disconnected" | "connecting" | "connected" | "error";
export interface RelayConnectionOptions {
    channel: string;
    host?: string;
    port?: number;
    tls?: boolean;
    deviceName?: string;
}
export declare class RelayConnection {
    private ws;
    private heartbeatTimer;
    private _state;
    private readonly channel;
    private readonly host;
    private readonly tls;
    private readonly deviceName;
    /** Called when a binary packet arrives (raw RTP/OSTP). */
    onPacket: ((data: Uint8Array) => void) | null;
    /** Called when a text control message arrives. */
    onControlMessage: ((msg: string) => void) | null;
    /** Called when connection state changes. */
    onStateChange: ((state: RelayConnectionState) => void) | null;
    get state(): RelayConnectionState;
    constructor(options: RelayConnectionOptions);
    /** Connect to the relay via WebSocket. */
    connect(): void;
    /** Disconnect from the relay. */
    disconnect(): void;
    private startHeartbeat;
    private stopHeartbeat;
    private sendText;
    private setState;
}
