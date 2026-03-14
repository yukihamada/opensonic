/**
 * soluna.js — OSTP JavaScript Receiver SDK
 * Open Sonic Transport Protocol browser client
 *
 * Connects to wss://relay.solun.art/ws/audio?channel=<name>&format=raw
 * Decodes Opus frames via Web Codecs API (AudioDecoder) with fallback.
 * Plays decoded audio via Web Audio API with adaptive jitter buffer.
 * Implements basic swarm signaling (SWARM_READY on WiFi, SWARM_UNABLE on cellular).
 *
 * Browser support: Chrome 94+, Firefox 115+, Safari 16.4+
 *
 * @module soluna
 * @version 0.9.0
 */

const RELAY_BASE = 'wss://relay.solun.art';
const SWARM_BASE = 'wss://relay.solun.art';
const BOOTSTRAP_BASE = 'https://relay.solun.art';

/** Default jitter buffer sizes in seconds by connection type. */
const JITTER_BUFFER = {
  wifi: 0.080,
  cellular: 0.200,
  unknown: 0.120,
};

/** Swarm role states. */
const SwarmState = {
  IDLE: 'IDLE',
  QUERIED: 'QUERIED',
  ASSIGNED: 'ASSIGNED',
  CONNECTING: 'CONNECTING',
  ACTIVE: 'ACTIVE',
  FAILED: 'FAILED',
};

/**
 * Detect whether the browser is on a cellular/metered connection.
 * Uses the Network Information API where available.
 * @returns {'wifi'|'cellular'|'unknown'}
 */
function detectConnectionType() {
  const conn = navigator.connection || navigator.mozConnection || navigator.webkitConnection;
  if (!conn) return 'unknown';
  const type = conn.effectiveType || conn.type || '';
  if (['2g', '3g', 'cellular'].includes(type)) return 'cellular';
  if (type === 'wifi' || type === 'ethernet') return 'wifi';
  return 'unknown';
}

/**
 * Detect whether the browser is on a mobile device.
 * @returns {boolean}
 */
function detectMobile() {
  return /Mobi|Android|iPhone|iPad|iPod/i.test(navigator.userAgent);
}

/**
 * @typedef {Object} SolunaOptions
 * @property {string} [relayBase='wss://relay.solun.art'] - WebSocket relay base URL.
 * @property {string} [bootstrapBase='https://relay.solun.art'] - HTTP bootstrap base URL.
 * @property {string} [token] - Session token (from bootstrap). Optional; SDK fetches if absent.
 * @property {number} [targetLatencyMs=80] - Target audio latency in milliseconds.
 * @property {boolean} [enableSwarm=true] - Participate in swarm signaling.
 * @property {boolean} [verbose=false] - Log debug info to console.
 */

/**
 * OSTP receiver for browsers.
 *
 * @example
 * const rx = new SolunaReceiver({ verbose: true });
 * rx.on('connected', info => console.log('Live:', info));
 * rx.on('status', s => updateUI(s));
 * await rx.connect('soluna/stage-a');
 */
export class SolunaReceiver {
  /**
   * @param {SolunaOptions} [options={}]
   */
  constructor(options = {}) {
    this._opts = {
      relayBase: RELAY_BASE,
      bootstrapBase: BOOTSTRAP_BASE,
      token: null,
      targetLatencyMs: 80,
      enableSwarm: true,
      verbose: false,
      ...options,
    };

    /** @type {Map<string, Function[]>} */
    this._handlers = new Map();

    this._audioCtx = null;
    this._decoder = null;
    this._ws = null;        // audio WebSocket
    this._swarmWs = null;   // swarm signaling WebSocket
    this._channel = null;
    this._streamInfo = null;
    this._connectionType = detectConnectionType();
    this._isMobile = detectMobile();

    // Jitter buffer
    this._bufferQueue = [];
    this._bufferDuration = 0; // seconds of audio in buffer
    this._targetBuffer = (JITTER_BUFFER[this._connectionType] || JITTER_BUFFER.unknown);
    this._playing = false;
    this._nextPlayTime = 0;

    // Swarm state
    this._swarmState = SwarmState.IDLE;
    this._swarmRequestId = null;
  }

  // ---------------------------------------------------------------------------
  // Public API
  // ---------------------------------------------------------------------------

  /**
   * Connect to a channel and begin playback.
   * @param {string} channel - Channel name, e.g. 'soluna/stage-a'.
   * @returns {Promise<void>} Resolves when the WebSocket connection is open.
   */
  async connect(channel) {
    if (this._ws) {
      this._log('Already connected; disconnecting first.');
      this.disconnect();
    }

    this._channel = channel;
    this._log(`Connecting to channel: ${channel}`);

    // Refresh connection type in case network changed
    this._connectionType = detectConnectionType();
    this._targetBuffer = JITTER_BUFFER[this._connectionType] || JITTER_BUFFER.unknown;

    // Optionally fetch bootstrap for metadata
    await this._bootstrap(channel).catch(err => {
      this._log('Bootstrap fetch failed (non-fatal):', err.message);
    });

    await this._initAudio();
    this._openAudioSocket(channel);

    if (this._opts.enableSwarm) {
      this._openSwarmSocket(channel);
    }
  }

  /**
   * Disconnect from the current channel and stop playback.
   */
  disconnect() {
    this._log('Disconnecting.');
    this._playing = false;

    if (this._ws) {
      this._ws.onclose = null; // prevent auto-reconnect on intentional close
      this._ws.close(1000, 'client disconnect');
      this._ws = null;
    }

    if (this._swarmWs) {
      this._swarmWs.close(1000, 'client disconnect');
      this._swarmWs = null;
    }

    if (this._decoder) {
      this._decoder.close();
      this._decoder = null;
    }

    if (this._audioCtx && this._audioCtx.state !== 'closed') {
      this._audioCtx.close();
      this._audioCtx = null;
    }

    this._bufferQueue = [];
    this._bufferDuration = 0;
    this._swarmState = SwarmState.IDLE;
    this._channel = null;
    this._streamInfo = null;

    this._emit('disconnected', { reason: 'client' });
  }

  /**
   * Register an event handler.
   * @param {'connected'|'disconnected'|'error'|'status'} event
   * @param {Function} handler
   * @returns {this}
   */
  on(event, handler) {
    if (!this._handlers.has(event)) this._handlers.set(event, []);
    this._handlers.get(event).push(handler);
    return this;
  }

  /**
   * Remove an event handler, or all handlers for an event if handler is omitted.
   * @param {string} event
   * @param {Function} [handler]
   * @returns {this}
   */
  off(event, handler) {
    if (!handler) {
      this._handlers.delete(event);
    } else {
      const list = this._handlers.get(event) || [];
      this._handlers.set(event, list.filter(h => h !== handler));
    }
    return this;
  }

  // ---------------------------------------------------------------------------
  // Internal: Bootstrap
  // ---------------------------------------------------------------------------

  async _bootstrap(channel) {
    const encoded = encodeURIComponent(channel);
    const url = `${this._opts.bootstrapBase}/api/channel/${encoded}/bootstrap`;
    const res = await fetch(url);
    if (!res.ok) throw new Error(`Bootstrap HTTP ${res.status}`);
    const data = await res.json();
    this._streamInfo = data;
    this._log('Bootstrap:', data);
    return data;
  }

  // ---------------------------------------------------------------------------
  // Internal: Audio Initialization
  // ---------------------------------------------------------------------------

  async _initAudio() {
    if (this._audioCtx) return; // already initialized

    this._audioCtx = new (window.AudioContext || window.webkitAudioContext)({
      sampleRate: this._streamInfo?.sample_rate || 48000,
      latencyHint: 'interactive',
    });

    // Resume AudioContext on user gesture (required by browsers)
    if (this._audioCtx.state === 'suspended') {
      await this._audioCtx.resume();
    }

    if (typeof AudioDecoder !== 'undefined') {
      await this._initWebCodecsDecoder();
    } else {
      this._log('AudioDecoder not available; using ScriptProcessor fallback');
      await this._initFallbackDecoder();
    }
  }

  async _initWebCodecsDecoder() {
    const sampleRate = this._streamInfo?.sample_rate || 48000;
    const numChannels = this._streamInfo?.channels || 2;

    // Check if Opus is supported
    const support = await AudioDecoder.isConfigSupported({
      codec: 'opus',
      sampleRate,
      numberOfChannels: numChannels,
    });

    if (!support.supported) {
      this._log('Opus AudioDecoder not supported; using fallback');
      await this._initFallbackDecoder();
      return;
    }

    this._decoder = new AudioDecoder({
      output: (audioData) => this._onDecodedFrame(audioData),
      error: (err) => {
        this._emit('error', { type: 'decoder', message: err.message });
        this._log('Decoder error:', err);
      },
    });

    this._decoder.configure({
      codec: 'opus',
      sampleRate,
      numberOfChannels: numChannels,
    });

    this._log(`AudioDecoder configured: Opus ${sampleRate}Hz ${numChannels}ch`);
  }

  async _initFallbackDecoder() {
    // Minimal fallback: signal that we're in degraded mode.
    // Full Opus WASM decoder integration would go here.
    this._emit('status', {
      type: 'warning',
      message: 'AudioDecoder API unavailable; playback may not work in this browser.',
    });
    this._decoder = null;
  }

  // ---------------------------------------------------------------------------
  // Internal: Audio WebSocket
  // ---------------------------------------------------------------------------

  _openAudioSocket(channel) {
    const params = new URLSearchParams({
      channel,
      format: 'raw',
    });
    if (this._opts.token) params.set('token', this._opts.token);

    const url = `${this._opts.relayBase}/ws/audio?${params}`;
    this._log('Opening audio WebSocket:', url);

    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    this._ws = ws;

    ws.onopen = () => {
      this._log('Audio WebSocket open');
      this._emit('connected', {
        channel,
        connectionType: this._connectionType,
        targetLatencyMs: Math.round(this._targetBuffer * 1000),
        streamInfo: this._streamInfo,
      });
      this._emit('status', { type: 'info', message: 'Connected' });
    };

    ws.onmessage = (event) => {
      if (typeof event.data === 'string') {
        this._handleTextFrame(event.data);
      } else {
        this._handleOpusFrame(event.data);
      }
    };

    ws.onerror = (event) => {
      this._emit('error', { type: 'websocket', message: 'WebSocket error' });
      this._log('WebSocket error', event);
    };

    ws.onclose = (event) => {
      this._log(`Audio WebSocket closed: code=${event.code} reason=${event.reason}`);
      this._emit('disconnected', { code: event.code, reason: event.reason });
      this._playing = false;
      this._ws = null;
    };
  }

  _handleTextFrame(data) {
    try {
      const msg = JSON.parse(data);
      if (msg.type === 'stream_info') {
        this._streamInfo = { ...this._streamInfo, ...msg };
        this._log('Stream info:', msg);
        this._emit('status', {
          type: 'info',
          message: `Stream: ${msg.bitrate_kbps || '?'} kbps, ${msg.sample_rate || 48000} Hz`,
          streamInfo: msg,
        });
      }
    } catch (e) {
      this._log('Failed to parse text frame:', e);
    }
  }

  _handleOpusFrame(buffer) {
    if (!this._decoder || this._decoder.state === 'closed') return;

    const frameDurationUs = ((this._streamInfo?.frame_duration_ms || 20) * 1000);

    // Enqueue an EncodedAudioChunk for the WebCodecs decoder
    const chunk = new EncodedAudioChunk({
      type: 'key', // Opus frames are self-contained
      timestamp: this._nextDecodeTimestamp || 0,
      duration: frameDurationUs,
      data: buffer,
    });

    this._nextDecodeTimestamp = (this._nextDecodeTimestamp || 0) + frameDurationUs;

    try {
      this._decoder.decode(chunk);
    } catch (err) {
      this._log('Decode error:', err);
    }
  }

  // ---------------------------------------------------------------------------
  // Internal: Decoded audio output → Web Audio API
  // ---------------------------------------------------------------------------

  _onDecodedFrame(audioData) {
    const ctx = this._audioCtx;
    if (!ctx) { audioData.close(); return; }

    const numChannels = audioData.numberOfChannels;
    const numFrames = audioData.numberOfFrames;
    const sampleRate = audioData.sampleRate;

    // Copy AudioData into an AudioBuffer
    const audioBuffer = ctx.createBuffer(numChannels, numFrames, sampleRate);
    for (let ch = 0; ch < numChannels; ch++) {
      const channelData = audioBuffer.getChannelData(ch);
      audioData.copyTo(channelData, { planeIndex: ch });
    }
    audioData.close();

    const frameDuration = numFrames / sampleRate;
    this._bufferDuration += frameDuration;

    // Buffering phase: accumulate target latency before starting playback
    if (!this._playing) {
      this._bufferQueue.push(audioBuffer);
      if (this._bufferDuration >= this._targetBuffer) {
        this._log(`Buffer full (${Math.round(this._bufferDuration * 1000)}ms), starting playback`);
        this._startPlayback();
      }
      return;
    }

    // Playback phase: schedule this buffer immediately
    this._scheduleBuffer(audioBuffer);
  }

  _startPlayback() {
    this._playing = true;
    this._nextPlayTime = this._audioCtx.currentTime;

    this._emit('status', { type: 'info', message: 'Playback started' });

    for (const buf of this._bufferQueue) {
      this._scheduleBuffer(buf);
    }
    this._bufferQueue = [];
  }

  _scheduleBuffer(audioBuffer) {
    const ctx = this._audioCtx;
    const source = ctx.createBufferSource();
    source.buffer = audioBuffer;
    source.connect(ctx.destination);

    const startTime = Math.max(ctx.currentTime, this._nextPlayTime);
    source.start(startTime);

    this._nextPlayTime = startTime + audioBuffer.duration;
    this._bufferDuration -= audioBuffer.duration;
  }

  // ---------------------------------------------------------------------------
  // Internal: Swarm WebSocket
  // ---------------------------------------------------------------------------

  _openSwarmSocket(channel) {
    const params = new URLSearchParams({ channel });
    if (this._opts.token) params.set('token', this._opts.token);

    const url = `${this._opts.relayBase}/ws/swarm?${params}`;
    this._log('Opening swarm WebSocket:', url);

    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    this._swarmWs = ws;

    ws.onmessage = (event) => {
      if (typeof event.data !== 'string') return;
      try {
        const msg = JSON.parse(event.data);
        this._handleSwarmMessage(msg);
      } catch (e) {
        this._log('Swarm parse error:', e);
      }
    };

    ws.onerror = () => {
      this._log('Swarm WebSocket error (non-fatal)');
    };

    ws.onclose = () => {
      this._log('Swarm WebSocket closed');
      this._swarmWs = null;
    };
  }

  _handleSwarmMessage(msg) {
    this._log('Swarm message:', msg.type);

    switch (msg.type) {
      case 'SWARM_QUERY':
        this._swarmState = SwarmState.QUERIED;
        this._swarmRequestId = msg.request_id;
        this._respondToSwarmQuery(msg);
        break;

      case 'SWARM_ASSIGN':
        this._swarmState = SwarmState.ASSIGNED;
        this._log('Swarm assigned as leaf (browser limitation)');
        this._sendSwarm({
          type: 'SWARM_ACK',
          channel: this._channel,
          role: msg.role,
          primary_connected: false,
          secondary_connected: false,
          latency_primary_ms: 0,
          latency_secondary_ms: 0,
        });
        this._swarmState = SwarmState.ACTIVE;
        break;

      case 'SWARM_TEARDOWN':
        this._log('Swarm teardown:', msg.reason);
        this._swarmState = SwarmState.IDLE;
        if (msg.reason === 'source_stopped') {
          this._emit('status', { type: 'info', message: 'Stream ended' });
        }
        break;

      default:
        this._log('Unknown swarm message type:', msg.type);
    }
  }

  _respondToSwarmQuery(query) {
    const connType = this._connectionType;
    const isMobile = this._isMobile;

    // Browser clients: always decline relay duty.
    // Report SWARM_READY only on non-cellular non-mobile, so the SC knows
    // the client is a candidate for Leaf (not relay).
    if (connType === 'cellular' || isMobile) {
      this._sendSwarm({
        type: 'SWARM_UNABLE',
        channel: this._channel,
        request_id: query.request_id,
        reason: connType === 'cellular' ? 'cellular' : 'user_declined',
      });
      this._swarmState = SwarmState.IDLE;
    } else {
      // WiFi desktop: can receive in swarm leaf position, but no relay duty
      this._sendSwarm({
        type: 'SWARM_READY',
        channel: this._channel,
        request_id: query.request_id,
        upload_kbps: 0,         // 0 = leaf only, no uplink offered
        rtt_to_source_ms: null,
        nat_type: 'unknown',
        os: 'browser',
        udp_port: 0,            // browser cannot receive raw UDP
      });
    }
  }

  _sendSwarm(msg) {
    if (this._swarmWs && this._swarmWs.readyState === WebSocket.OPEN) {
      this._swarmWs.send(JSON.stringify(msg));
    }
  }

  // ---------------------------------------------------------------------------
  // Internal: Utilities
  // ---------------------------------------------------------------------------

  _emit(event, data) {
    const handlers = this._handlers.get(event) || [];
    for (const h of handlers) {
      try { h(data); } catch (e) { /* handler errors must not crash receiver */ }
    }
  }

  _log(...args) {
    if (this._opts.verbose) console.log('[SolunaReceiver]', ...args);
  }
}

export default SolunaReceiver;
