/**
 * WebSocket connection to the Soluna relay server.
 *
 * Browsers cannot use raw UDP sockets, so we connect via the relay's
 * WebSocket endpoint: wss://relay.solun.art/ws/audio?channel=NAME
 * Binary frames contain raw RTP/OSTP packets.
 */

import { OSTConstants } from "./OSTPacketParser.js";

export type RelayConnectionState = "disconnected" | "connecting" | "connected" | "error";

export interface RelayConnectionOptions {
  channel: string;
  host?: string;
  port?: number;
  tls?: boolean;
  deviceName?: string;
}

export class RelayConnection {
  private ws: WebSocket | null = null;
  private heartbeatTimer: ReturnType<typeof setInterval> | null = null;
  private _state: RelayConnectionState = "disconnected";

  private readonly channel: string;
  private readonly host: string;
  private readonly tls: boolean;
  private readonly deviceName: string;

  /** Called when a binary packet arrives (raw RTP/OSTP). */
  onPacket: ((data: Uint8Array) => void) | null = null;

  /** Called when a text control message arrives. */
  onControlMessage: ((msg: string) => void) | null = null;

  /** Called when connection state changes. */
  onStateChange: ((state: RelayConnectionState) => void) | null = null;

  get state(): RelayConnectionState {
    return this._state;
  }

  constructor(options: RelayConnectionOptions) {
    this.channel = options.channel;
    this.host = options.host ?? OSTConstants.defaultHost;
    this.tls = options.tls ?? true;
    this.deviceName = options.deviceName ?? "SolunaSDK-JS";
  }

  /** Connect to the relay via WebSocket. */
  connect(): void {
    if (this._state === "connected" || this._state === "connecting") return;

    this.setState("connecting");

    const scheme = this.tls ? "wss" : "ws";
    const url = `${scheme}://${this.host}/ws/audio?channel=${encodeURIComponent(this.channel)}`;

    this.ws = new WebSocket(url);
    this.ws.binaryType = "arraybuffer";

    this.ws.onopen = () => {
      this.setState("connected");
      this.startHeartbeat();
    };

    this.ws.onmessage = (event: MessageEvent) => {
      if (event.data instanceof ArrayBuffer) {
        const data = new Uint8Array(event.data);
        if (data.length < OSTConstants.rtpHeaderSize) return;

        // RTP/OSTP audio packet: (byte[0] & 0xC0) == 0x80
        if ((data[0] & 0xC0) === 0x80) {
          this.onPacket?.(data);
        } else {
          // Text control message
          const decoder = new TextDecoder();
          this.onControlMessage?.(decoder.decode(data));
        }
      } else if (typeof event.data === "string") {
        this.onControlMessage?.(event.data);
      }
    };

    this.ws.onerror = () => {
      this.setState("error");
    };

    this.ws.onclose = () => {
      this.stopHeartbeat();
      this.setState("disconnected");
    };
  }

  /** Disconnect from the relay. */
  disconnect(): void {
    this.stopHeartbeat();
    if (this.ws) {
      this.ws.onclose = null; // prevent state change callback
      this.ws.close();
      this.ws = null;
    }
    this.setState("disconnected");
  }

  private startHeartbeat(): void {
    this.heartbeatTimer = setInterval(() => {
      this.sendText(`HELLO\n`);
      this.sendText(`JOIN:${this.channel}::${this.deviceName}\n`);
    }, OSTConstants.heartbeatIntervalMs);
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer !== null) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = null;
    }
  }

  private sendText(msg: string): void {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(msg);
    }
  }

  private setState(state: RelayConnectionState): void {
    if (this._state !== state) {
      this._state = state;
      this.onStateChange?.(state);
    }
  }
}
