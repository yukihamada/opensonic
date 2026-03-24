/**
 * soluna.js — OSTP JavaScript Receiver + Sender SDK
 * Open Sonic Transport Protocol browser client
 *
 * SolunaReceiver:
 *   Connects to wss://relay.solun.art/ws/audio?channel=<name>&format=raw
 *   Decodes Opus frames via Web Codecs API (AudioDecoder) with fallback.
 *   Plays decoded audio via Web Audio API with adaptive jitter buffer.
 *   Implements basic swarm signaling (SWARM_READY on WiFi, SWARM_UNABLE on cellular).
 *   NACK: sends binary NACK requests when sequence gaps are detected.
 *   RTCP: sends JSON receiver reports every 5 seconds.
 *
 * SolunaSender:
 *   Captures microphone audio, encodes as raw PCM, sends via WebSocket.
 *   FEC: XOR parity packet every 5 audio packets (PT=127).
 *   RTCP: sends JSON sender/receiver reports every 5 seconds.
 *
 * Browser support: Chrome 94+, Firefox 115+, Safari 16.4+
 *
 * @module soluna
 * @version 0.10.0
 */

const RELAY_BASE      = 'wss://relay.solun.art';
const SWARM_BASE      = 'wss://relay.solun.art';
const BOOTSTRAP_BASE  = 'https://relay.solun.art';

const FEC_GROUP_SIZE  = 5;

/** Default jitter buffer sizes in seconds by connection type. */
const JITTER_BUFFER = {
  wifi:     0.080,
  cellular: 0.200,
  unknown:  0.120,
};

/** Swarm role states. */
const SwarmState = {
  IDLE:        'IDLE',
  QUERIED:     'QUERIED',
  ASSIGNED:    'ASSIGNED',
  CONNECTING:  'CONNECTING',
  ACTIVE:      'ACTIVE',
  FAILED:      'FAILED',
};

/**
 * Detect whether the browser is on a cellular/metered connection.
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

// ---------------------------------------------------------------------------
// Shared CRC-32 utility
// ---------------------------------------------------------------------------

const _CRC32_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    t[i] = c;
  }
  return t;
})();

function crc32(data) {
  let crc = 0xFFFFFFFF;
  for (let i = 0; i < data.length; i++) {
    crc = _CRC32_TABLE[(crc ^ data[i]) & 0xFF] ^ (crc >>> 8);
  }
  return (~crc) >>> 0;
}

// ---------------------------------------------------------------------------
// SolunaSender — microphone capture → OSTP/RTP → WebSocket relay
// ---------------------------------------------------------------------------

/**
 * @typedef {Object} SolunaSenderOptions
 * @property {string} [channel='default']         - Channel name.
 * @property {string} [password='']               - Optional channel password.
 * @property {string} [wsUrl='wss://relay.solun.art'] - Relay WebSocket base URL.
 */

/**
 * OSTP sender for browsers.
 *
 * @example
 * const tx = new SolunaSender({ channel: 'soluna/stage-a' });
 * await tx.start();
 * // later:
 * tx.stop();
 */
export class SolunaSender {
  /**
   * @param {SolunaSenderOptions} [options={}]
   */
  constructor(options = {}) {
    this._channel     = options.channel  || 'default';
    this._password    = options.password || '';
    this._wsUrl       = options.wsUrl    || 'wss://relay.solun.art';
    this._ws          = null;
    this._stream      = null;
    this._audioContext = null;
    this._worklet     = null;
    this._seq         = 0;
    this._ssrc        = (Math.random() * 0xFFFFFFFF) >>> 0;
    this._timestamp   = 0;
    this._fecBuf      = [];
    this._fecGroupId  = 0;
    this._pktsReceived = 0;
    this._pktsLost    = 0;
    this._lastSeq     = -1;
    this._jitterMs    = 0;
    this._rtcpInterval = null;
  }

  /**
   * Acquire microphone, open WebSocket, start audio pipeline and RTCP timer.
   * @returns {Promise<void>}
   */
  async start() {
    // 1. Acquire microphone
    this._stream = await navigator.mediaDevices.getUserMedia({
      audio: {
        sampleRate: 48000,
        channelCount: 1,
        echoCancellation: true,
      },
    });

    // 2. Connect to relay
    this._ws = new WebSocket(
      `${this._wsUrl}/ws/audio/publish?channel=${encodeURIComponent(this._channel)}&format=raw`
    );
    this._ws.binaryType = 'arraybuffer';

    await new Promise((resolve, reject) => {
      this._ws.onopen  = resolve;
      this._ws.onerror = reject;
    });

    // 3. Audio pipeline
    this._audioContext = new AudioContext({ sampleRate: 48000 });
    const source = this._audioContext.createMediaStreamSource(this._stream);

    await this._audioContext.audioWorklet.addModule('/wasm/soluna-sender-worklet.js');
    this._worklet = new AudioWorkletNode(this._audioContext, 'soluna-sender-processor');
    this._worklet.port.onmessage = (e) => this._onPcmFrame(e.data);
    source.connect(this._worklet);

    // 4. Start RTCP
    this._rtcpInterval = setInterval(() => this._sendRtcp(), 5000);
  }

  /** Stop all audio and close the WebSocket. */
  stop() {
    clearInterval(this._rtcpInterval);
    this._worklet?.disconnect();
    this._audioContext?.close();
    this._stream?.getTracks().forEach(t => t.stop());
    this._ws?.close();
    this._ws          = null;
    this._stream      = null;
    this._audioContext = null;
    this._worklet     = null;
  }

  // -------------------------------------------------------------------------
  // Internal: PCM frame handler
  // -------------------------------------------------------------------------

  /**
   * Called by the AudioWorklet with 960 Float32 samples (20 ms at 48 kHz).
   * @param {Float32Array} pcm
   */
  _onPcmFrame(pcm) {
    const packet = this._buildOstpPacket(pcm);
    if (this._ws?.readyState === WebSocket.OPEN) {
      this._ws.send(packet);
    }

    // FEC: accumulate, send XOR parity every FEC_GROUP_SIZE packets
    this._fecBuf.push(new Uint8Array(packet));
    if (this._fecBuf.length >= FEC_GROUP_SIZE) {
      const fecPkt = this._buildFecParity();
      if (this._ws?.readyState === WebSocket.OPEN) {
        this._ws.send(fecPkt);
      }
      this._fecBuf = [];
      this._fecGroupId++;
    }
  }

  // -------------------------------------------------------------------------
  // Internal: Packet construction
  // -------------------------------------------------------------------------

  /**
   * Build an OSTP/RTP packet from raw Float32 PCM.
   * Layout: RTP(12) + ext-hdr(4) + OSTP-ext(8) + PCM-payload + CRC32(4)
   * @param {Float32Array} pcm
   * @returns {ArrayBuffer}
   */
  _buildOstpPacket(pcm) {
    const samples     = pcm.length;
    const payloadSize = samples * 2; // Int16 PCM
    const totalSize   = 12 + 4 + 8 + payloadSize + 4;
    const buf  = new ArrayBuffer(totalSize);
    const view = new DataView(buf);

    // RTP fixed header
    view.setUint8(0,  0x90);                          // V=2, P=0, X=1, CC=0
    view.setUint8(1,  96);                            // M=0, PT=96 (PCM24 dynamic)
    view.setUint16(2, this._seq++ & 0xFFFF, false);
    view.setUint32(4, this._timestamp, false);
    view.setUint32(8, this._ssrc, false);
    this._timestamp = (this._timestamp + 960) >>> 0;

    // RTP extension header: profile 'OS', length=2 (2×32-bit = 8 bytes)
    view.setUint16(12, 0x4F53, false); // 'OS'
    view.setUint16(14, 0x0002, false); // length = 2 words

    // OSTP extension
    view.setUint16(16, 0x0100, false); // stream_id: deck=0, ch=1 (mono)
    view.setUint16(18, this._fecGroupId & 0xFFFF, false); // sequence_ext = fec group
    view.setUint32(20, this._timestamp, false);            // media_timestamp

    // Int16 PCM payload
    const payloadOffset = 24;
    for (let i = 0; i < samples; i++) {
      const s = Math.max(-1, Math.min(1, pcm[i]));
      view.setInt16(payloadOffset + i * 2, (s * 32767) | 0, false);
    }

    // CRC-32
    const crc = crc32(new Uint8Array(buf, 0, totalSize - 4));
    view.setUint32(totalSize - 4, crc, false);

    return buf;
  }

  /**
   * Build a FEC parity packet (PT=127) from accumulated _fecBuf.
   * @returns {ArrayBuffer}
   */
  _buildFecParity() {
    const maxLen = Math.max(...this._fecBuf.map(p => p.length));
    const parity = new Uint8Array(maxLen);
    for (const pkt of this._fecBuf) {
      for (let i = 0; i < pkt.length; i++) parity[i] ^= pkt[i];
    }

    const totalSize = 12 + 4 + 8 + maxLen + 4;
    const buf  = new ArrayBuffer(totalSize);
    const view = new DataView(buf);

    view.setUint8(0,  0x90);
    view.setUint8(1,  127); // PT=127 FEC
    view.setUint16(2, this._seq++ & 0xFFFF, false);
    view.setUint32(4, this._timestamp, false);
    view.setUint32(8, this._ssrc, false);
    view.setUint16(12, 0x4F53, false);
    view.setUint16(14, 0x0002, false);
    view.setUint16(16, 0xFF00, false); // stream_id = FEC marker (0xFF00)
    view.setUint16(18, this._fecGroupId & 0xFFFF, false);
    view.setUint32(20, this._timestamp, false);
    new Uint8Array(buf, 24, maxLen).set(parity);

    const crc = crc32(new Uint8Array(buf, 0, totalSize - 4));
    view.setUint32(totalSize - 4, crc, false);

    return buf;
  }

  // -------------------------------------------------------------------------
  // Internal: RTCP
  // -------------------------------------------------------------------------

  _sendRtcp() {
    if (this._ws?.readyState !== WebSocket.OPEN) return;
    const report = JSON.stringify({
      type:      'receiver_report',
      ssrc:      this._ssrc,
      pkts_rx:   this._pktsReceived,
      pkts_lost: this._pktsLost,
      jitter_ms: parseFloat(this._jitterMs.toFixed(2)),
      last_seq:  this._lastSeq,
    });
    this._ws.send(report);
  }
}

// ---------------------------------------------------------------------------
// SolunaReceiver — WebSocket relay → decode → Web Audio API
// ---------------------------------------------------------------------------

/**
 * @typedef {Object} SolunaOptions
 * @property {string}  [relayBase='wss://relay.solun.art']    - WebSocket relay base URL.
 * @property {string}  [bootstrapBase='https://relay.solun.art'] - HTTP bootstrap base URL.
 * @property {string}  [token]           - Session token. Optional; SDK fetches if absent.
 * @property {number}  [targetLatencyMs=80] - Target audio latency in milliseconds.
 * @property {boolean} [enableSwarm=true]   - Participate in swarm signaling.
 * @property {boolean} [verbose=false]      - Log debug info to console.
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
      relayBase:      RELAY_BASE,
      bootstrapBase:  BOOTSTRAP_BASE,
      token:          null,
      targetLatencyMs: 80,
      enableSwarm:    true,
      verbose:        false,
      ...options,
    };

    /** @type {Map<string, Function[]>} */
    this._handlers = new Map();

    this._audioCtx    = null;
    this._decoder     = null;
    this._ws          = null;
    this._swarmWs     = null;
    this._channel     = null;
    this._streamInfo  = null;
    this._connectionType = detectConnectionType();
    this._isMobile    = detectMobile();

    // Jitter buffer
    this._bufferQueue    = [];
    this._bufferDuration = 0;
    this._targetBuffer   = JITTER_BUFFER[this._connectionType] || JITTER_BUFFER.unknown;
    this._playing        = false;
    this._nextPlayTime   = 0;

    // Swarm state
    this._swarmState     = SwarmState.IDLE;
    this._swarmRequestId = null;

    // Sequence tracking for NACK + RTCP
    this._lastRxSeq      = -1;
    this._pktsReceived   = 0;
    this._pktsLost       = 0;
    this._jitterMs       = 0;
    this._lastPktTime    = 0;
    this._rtcpInterval   = null;
  }

  // ---------------------------------------------------------------------------
  // Public API
  // ---------------------------------------------------------------------------

  /**
   * Connect to a channel and begin playback.
   * @param {string} channel
   * @returns {Promise<void>}
   */
  async connect(channel) {
    if (this._ws) {
      this._log('Already connected; disconnecting first.');
      this.disconnect();
    }

    this._channel = channel;
    this._log(`Connecting to channel: ${channel}`);

    this._connectionType = detectConnectionType();
    this._targetBuffer   = JITTER_BUFFER[this._connectionType] || JITTER_BUFFER.unknown;

    await this._bootstrap(channel).catch(err => {
      this._log('Bootstrap fetch failed (non-fatal):', err.message);
    });

    await this._initAudio();
    this._openAudioSocket(channel);

    if (this._opts.enableSwarm) {
      this._openSwarmSocket(channel);
    }
  }

  /** Disconnect from the current channel and stop playback. */
  disconnect() {
    this._log('Disconnecting.');
    this._playing = false;

    clearInterval(this._rtcpInterval);
    this._rtcpInterval = null;

    if (this._ws) {
      this._ws.onclose = null;
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

    this._bufferQueue    = [];
    this._bufferDuration = 0;
    this._swarmState     = SwarmState.IDLE;
    this._channel        = null;
    this._streamInfo     = null;
    this._lastRxSeq      = -1;
    this._pktsReceived   = 0;
    this._pktsLost       = 0;

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
   * Remove an event handler, or all handlers if handler is omitted.
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
    const url     = `${this._opts.bootstrapBase}/api/channel/${encoded}/bootstrap`;
    const res     = await fetch(url);
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
    if (this._audioCtx) return;

    this._audioCtx = new (window.AudioContext || window.webkitAudioContext)({
      sampleRate:  this._streamInfo?.sample_rate || 48000,
      latencyHint: 'interactive',
    });

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
    const sampleRate  = this._streamInfo?.sample_rate  || 48000;
    const numChannels = this._streamInfo?.channels      || 2;

    const support = await AudioDecoder.isConfigSupported({
      codec:            'opus',
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
      error:  (err) => {
        this._emit('error', { type: 'decoder', message: err.message });
        this._log('Decoder error:', err);
      },
    });

    this._decoder.configure({ codec: 'opus', sampleRate, numberOfChannels: numChannels });
    this._log(`AudioDecoder configured: Opus ${sampleRate}Hz ${numChannels}ch`);
  }

  async _initFallbackDecoder() {
    this._emit('status', {
      type:    'warning',
      message: 'AudioDecoder API unavailable; playback may not work in this browser.',
    });
    this._decoder = null;
  }

  // ---------------------------------------------------------------------------
  // Internal: Audio WebSocket
  // ---------------------------------------------------------------------------

  _openAudioSocket(channel) {
    const params = new URLSearchParams({ channel, format: 'raw' });
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
        connectionType:  this._connectionType,
        targetLatencyMs: Math.round(this._targetBuffer * 1000),
        streamInfo:      this._streamInfo,
      });
      this._emit('status', { type: 'info', message: 'Connected' });

      // Start RTCP reporting
      this._rtcpInterval = setInterval(() => this._sendRtcp(), 5000);
    };

    ws.onmessage = (event) => {
      if (typeof event.data === 'string') {
        this._handleTextFrame(event.data);
      } else {
        this._handleBinaryFrame(event.data);
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
      this._ws      = null;
      clearInterval(this._rtcpInterval);
      this._rtcpInterval = null;
    };
  }

  _handleTextFrame(data) {
    try {
      const msg = JSON.parse(data);
      if (msg.type === 'stream_info') {
        this._streamInfo = { ...this._streamInfo, ...msg };
        this._log('Stream info:', msg);
        this._emit('status', {
          type:       'info',
          message:    `Stream: ${msg.bitrate_kbps || '?'} kbps, ${msg.sample_rate || 48000} Hz`,
          streamInfo: msg,
        });
      }
    } catch (e) {
      this._log('Failed to parse text frame:', e);
    }
  }

  /**
   * Handle a binary WebSocket frame.
   * Peeks at the RTP sequence number (bytes 2-3) for NACK gap detection,
   * then forwards to the Opus decoder.
   * @param {ArrayBuffer} buffer
   */
  _handleBinaryFrame(buffer) {
    // Minimum valid OSTP/RTP packet is 12 bytes (RTP header)
    if (buffer.byteLength >= 12) {
      const view = new DataView(buffer);
      const seq  = view.getUint16(2, false); // big-endian RTP seq

      const now = performance.now();

      if (this._lastRxSeq !== -1) {
        const expected = (this._lastRxSeq + 1) & 0xFFFF;
        if (seq !== expected) {
          // Gap detected — send NACK for missing sequence(s)
          let missing = expected;
          // Send up to 8 NACKs to avoid flooding
          let count = 0;
          while (missing !== seq && count < 8) {
            this._sendNack(missing);
            missing = (missing + 1) & 0xFFFF;
            this._pktsLost++;
            count++;
          }
        }

        // RFC 3550 inter-arrival jitter (simplified, ms)
        if (this._lastPktTime > 0) {
          const arrivalDiff = now - this._lastPktTime;
          const jitterDelta = Math.abs(arrivalDiff - 20); // 20 ms expected
          this._jitterMs += (jitterDelta - this._jitterMs) * 0.0625; // 1/16 smoothing
        }
      }

      this._lastPktTime = now;
      this._lastRxSeq   = seq;
      this._pktsReceived++;
    }

    this._handleOpusFrame(buffer);
  }

  /**
   * Send a NACK request for a missing sequence number.
   * Wire format: [0x4E, 0x41, seq_b3, seq_b2, seq_b1, seq_b0] (6 bytes)
   * @param {number} seq - 16-bit sequence number of the lost packet.
   */
  _sendNack(seq) {
    if (this._ws?.readyState !== WebSocket.OPEN) return;
    const buf  = new ArrayBuffer(6);
    const view = new DataView(buf);
    view.setUint8(0,  0x4E); // 'N'
    view.setUint8(1,  0x41); // 'A'
    // Store seq as uint32 big-endian (upper 16 bits = 0)
    view.setUint8(2,  0x00);
    view.setUint8(3,  0x00);
    view.setUint8(4,  (seq >> 8) & 0xFF);
    view.setUint8(5,   seq       & 0xFF);
    this._ws.send(buf);
    this._log(`NACK sent for seq=${seq}`);
  }

  /** Send a JSON RTCP receiver report over the audio WebSocket. */
  _sendRtcp() {
    if (this._ws?.readyState !== WebSocket.OPEN) return;
    const total      = this._pktsReceived + this._pktsLost;
    const loss_pct   = total > 0 ? (this._pktsLost / total * 100).toFixed(1) : '0.0';
    const report = JSON.stringify({
      type:      'receiver_report',
      pkts_rx:   this._pktsReceived,
      pkts_lost: this._pktsLost,
      loss_pct:  parseFloat(loss_pct),
      jitter_ms: parseFloat(this._jitterMs.toFixed(2)),
      last_seq:  this._lastRxSeq,
    });
    this._ws.send(report);
    this._log('RTCP sent:', report);
  }

  _handleOpusFrame(buffer) {
    if (!this._decoder || this._decoder.state === 'closed') return;

    const frameDurationUs = ((this._streamInfo?.frame_duration_ms || 20) * 1000);

    const chunk = new EncodedAudioChunk({
      type:      'key',
      timestamp:  this._nextDecodeTimestamp || 0,
      duration:   frameDurationUs,
      data:       buffer,
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
    const numFrames   = audioData.numberOfFrames;
    const sampleRate  = audioData.sampleRate;

    const audioBuffer = ctx.createBuffer(numChannels, numFrames, sampleRate);
    for (let ch = 0; ch < numChannels; ch++) {
      const channelData = audioBuffer.getChannelData(ch);
      audioData.copyTo(channelData, { planeIndex: ch });
    }
    audioData.close();

    const frameDuration   = numFrames / sampleRate;
    this._bufferDuration += frameDuration;

    if (!this._playing) {
      this._bufferQueue.push(audioBuffer);
      if (this._bufferDuration >= this._targetBuffer) {
        this._log(`Buffer full (${Math.round(this._bufferDuration * 1000)}ms), starting playback`);
        this._startPlayback();
      }
      return;
    }

    this._scheduleBuffer(audioBuffer);
  }

  _startPlayback() {
    this._playing      = true;
    this._nextPlayTime = this._audioCtx.currentTime;
    this._emit('status', { type: 'info', message: 'Playback started' });

    for (const buf of this._bufferQueue) {
      this._scheduleBuffer(buf);
    }
    this._bufferQueue = [];
  }

  _scheduleBuffer(audioBuffer) {
    const ctx    = this._audioCtx;
    const source = ctx.createBufferSource();
    source.buffer = audioBuffer;
    source.connect(ctx.destination);

    const startTime        = Math.max(ctx.currentTime, this._nextPlayTime);
    source.start(startTime);
    this._nextPlayTime     = startTime + audioBuffer.duration;
    this._bufferDuration  -= audioBuffer.duration;
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
        this._swarmState     = SwarmState.QUERIED;
        this._swarmRequestId = msg.request_id;
        this._respondToSwarmQuery(msg);
        break;

      case 'SWARM_ASSIGN':
        this._swarmState = SwarmState.ASSIGNED;
        this._log('Swarm assigned as leaf (browser limitation)');
        this._sendSwarm({
          type:                'SWARM_ACK',
          channel:             this._channel,
          role:                msg.role,
          primary_connected:   false,
          secondary_connected: false,
          latency_primary_ms:  0,
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

    if (connType === 'cellular') {
      // Cellular connections: leaf-only (carrier NAT, bandwidth constraints)
      this._sendSwarm({
        type:       'SWARM_UNABLE',
        channel:    this._channel,
        request_id: query.request_id,
        reason:     'cellular',
      });
      this._swarmState = SwarmState.IDLE;
    } else {
      // WiFi (desktop & mobile): volunteer for relay
      // Symmetric NAT nodes will be auto-detected by relay and switched to
      // relay-mediated mode (relay proxies to children on their behalf)
      this._sendSwarm({
        type:           'SWARM_READY',
        channel:        this._channel,
        request_id:     query.request_id,
        upload_kbps:    0,
        rtt_to_source_ms: null,
        nat_type:       'unknown',
        os:             'browser',
        udp_port:       0,
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
