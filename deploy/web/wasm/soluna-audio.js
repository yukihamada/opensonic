/**
 * Soluna WASM Audio Engine (non-module compatible)
 *
 * Loads .wasm via fetch() — works from any <script> tag.
 * Exposes: window.SolunaAudio (RX player) and window.SolunaTx (TX encoder)
 */
(function() {
    'use strict';

    const WASM_URL = '/wasm/pkg/soluna_wasm_bg.wasm';
    const WORKLET_URL = '/wasm/soluna-worklet.js';

    let wasmInstance = null;
    let wasmMemory = null;
    let cachedUint8 = null;
    let cachedFloat32 = null;
    let WASM_VECTOR_LEN = 0;

    function getUint8() {
        if (!cachedUint8 || cachedUint8.byteLength === 0) cachedUint8 = new Uint8Array(wasmMemory.buffer);
        return cachedUint8;
    }
    function getFloat32() {
        if (!cachedFloat32 || cachedFloat32.byteLength === 0) cachedFloat32 = new Float32Array(wasmMemory.buffer);
        return cachedFloat32;
    }
    function passArray8(arg, malloc) {
        const ptr = malloc(arg.length, 1) >>> 0;
        getUint8().set(arg, ptr);
        WASM_VECTOR_LEN = arg.length;
        return ptr;
    }
    function passArrayF32(arg, malloc) {
        const ptr = malloc(arg.length * 4, 4) >>> 0;
        getFloat32().set(arg, ptr / 4);
        WASM_VECTOR_LEN = arg.length;
        return ptr;
    }
    function getArrayU8(ptr, len) {
        return getUint8().subarray(ptr >>> 0, (ptr >>> 0) + len);
    }
    function getArrayF32(ptr, len) {
        ptr = ptr >>> 0;
        return getFloat32().subarray(ptr / 4, ptr / 4 + len);
    }

    // Minimal wasm-bindgen imports that the generated wasm expects
    function getImports() {
        return {
            __proto__: null,
            './soluna_wasm_bg.js': {
                __proto__: null,
                __wbg___wbindgen_copy_to_typed_array_d2f20acdab8e0740: function(arg0, arg1, arg2) {
                    new Uint8Array(arg2.buffer, arg2.byteOffset, arg2.byteLength).set(getArrayU8(arg0, arg1));
                },
                __wbg___wbindgen_throw_6ddd609b62940d55: function(arg0, arg1) {
                    const dec = new TextDecoder();
                    throw new Error(dec.decode(getUint8().subarray(arg0, arg0 + arg1)));
                },
                __wbg_now_16f0c993d5dd6c27: function() {
                    return Date.now();
                },
                // Opus bridge: decode callback (WASM → JS)
                __wbg_on_opus_packet_3cd0a052193e55de: function(arg0, arg1, arg2, arg3, arg4) {
                    if (globalThis.SolunaOpusBridge && globalThis.SolunaOpusBridge.on_opus_packet) {
                        globalThis.SolunaOpusBridge.on_opus_packet(getArrayU8(arg0, arg1), arg2, arg3 >>> 0, arg4 >>> 0);
                    }
                },
                // Opus bridge: encode callback (WASM → JS)
                __wbg_on_opus_encode_016dc3431f7bf0b5: function(arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7) {
                    if (globalThis.SolunaOpusBridge && globalThis.SolunaOpusBridge.on_opus_encode) {
                        globalThis.SolunaOpusBridge.on_opus_encode(getArrayF32(arg0, arg1), arg2 >>> 0, arg3 >>> 0, arg4, arg5 >>> 0, arg6 >>> 0, arg7);
                    }
                },
                __wbindgen_init_externref_table: function() {
                    const table = wasmInstance.exports.__wbindgen_externrefs;
                    const offset = table.grow(4);
                    table.set(0, undefined);
                    table.set(offset + 0, undefined);
                    table.set(offset + 1, null);
                    table.set(offset + 2, true);
                    table.set(offset + 3, false);
                },
            }
        };
    }

    async function loadWasm() {
        if (wasmInstance) return;
        const imports = getImports();
        const resp = fetch(WASM_URL);
        let result;
        if (typeof WebAssembly.instantiateStreaming === 'function') {
            try {
                result = await WebAssembly.instantiateStreaming(resp, imports);
            } catch (e) {
                // Fallback if MIME type wrong
                const bytes = await (await resp).arrayBuffer();
                result = await WebAssembly.instantiate(bytes, imports);
            }
        } else {
            const bytes = await (await resp).arrayBuffer();
            result = await WebAssembly.instantiate(bytes, imports);
        }
        wasmInstance = result.instance;
        wasmMemory = wasmInstance.exports.memory;
        cachedUint8 = null;
        cachedFloat32 = null;
        // Run wasm init
        wasmInstance.exports.__wbindgen_start();

        // Signal Opus bridge availability to WASM
        if (globalThis.SolunaOpusBridge && globalThis.SolunaOpusBridge.isSupported()) {
            wasmInstance.exports.set_opus_bridge_available(1);
            console.log('[SolunaAudio] Opus bridge detected and enabled');
        }
    }

    const w = () => wasmInstance.exports;

    // ── SolunaPlayer (RX) ──────────────────────────────────

    class SolunaPlayer {
        constructor(outChannels) {
            this._ptr = w().solunaplayer_new(outChannels || 2) >>> 0;
        }
        push_packet(data) {
            const u8 = data instanceof Uint8Array ? data : new Uint8Array(data);
            const ptr = passArray8(u8, w().__wbindgen_malloc);
            w().solunaplayer_push_packet(this._ptr, ptr, WASM_VECTOR_LEN);
        }
        push_packet_with_time(data, nowMs) {
            const u8 = data instanceof Uint8Array ? data : new Uint8Array(data);
            const ptr = passArray8(u8, w().__wbindgen_malloc);
            w().solunaplayer_push_packet_with_time(this._ptr, ptr, WASM_VECTOR_LEN, nowMs);
        }
        /** Push pre-decoded Opus f32 PCM (called from Opus bridge after WebCodecs decode). */
        push_decoded_opus(pcm, channels) {
            const ptr = passArrayF32(pcm, w().__wbindgen_malloc);
            w().solunaplayer_push_decoded_opus(this._ptr, ptr, WASM_VECTOR_LEN, channels);
        }
        pull_audio(outL, outR) {
            const ptrL = passArrayF32(outL, w().__wbindgen_malloc);
            const lenL = WASM_VECTOR_LEN;
            const ptrR = passArrayF32(outR, w().__wbindgen_malloc);
            const lenR = WASM_VECTOR_LEN;
            return w().solunaplayer_pull_audio(this._ptr, ptrL, lenL, outL, ptrR, lenR, outR) >>> 0;
        }
        available()              { return w().solunaplayer_available(this._ptr) >>> 0; }
        packet_count()           { return w().solunaplayer_packet_count(this._ptr) >>> 0; }
        underrun_count()         { return w().solunaplayer_underrun_count(this._ptr) >>> 0; }
        detected_tx_channels()   { return w().solunaplayer_detected_tx_channels(this._ptr) >>> 0; }
        opus_packet_count()      { return w().solunaplayer_opus_packet_count(this._ptr) >>> 0; }
        opus_decoded_count()     { return w().solunaplayer_opus_decoded_count(this._ptr) >>> 0; }
        set_output_channels(ch)  { w().solunaplayer_set_output_channels(this._ptr, ch); }
        set_sync_enabled(en)     { w().solunaplayer_set_sync_enabled(this._ptr, en); }
        set_sync_delay_ms(ms)    { w().solunaplayer_set_sync_delay_ms(this._ptr, ms); }
        sync_delay_ms()          { return w().solunaplayer_sync_delay_ms(this._ptr) >>> 0; }
        sync_locked()            { return w().solunaplayer_sync_locked(this._ptr) !== 0; }
        target_fill_frames()     { return w().solunaplayer_target_fill_frames(this._ptr) >>> 0; }
        net_delay_ms()           { return w().solunaplayer_net_delay_ms(this._ptr); }
        set_clock_offset(ms)     { w().solunaplayer_set_clock_offset(this._ptr, ms); }
        clock_offset_ms()        { return w().solunaplayer_clock_offset_ms(this._ptr); }
        clear()                  { w().solunaplayer_clear(this._ptr); }
        free()                   { w().__wbg_solunaplayer_free(this._ptr, 0); this._ptr = 0; }
    }

    // ── SolunaTx (TX) ──────────────────────────────────────

    class SolunaTxEncoder {
        constructor(channels, streamIdBase) {
            this._ptr = w().solunatx_new(channels || 1, streamIdBase || 1) >>> 0;
        }
        encode_frame(pcm) {
            const ptr = passArrayF32(pcm, w().__wbindgen_malloc);
            const ret = w().solunatx_encode_frame(this._ptr, ptr, WASM_VECTOR_LEN);
            const pkt = getArrayU8(ret[0], ret[1]).slice();
            w().__wbindgen_free(ret[0], ret[1], 1);
            return pkt;
        }
        /** Build OSTP packet from pre-encoded Opus bytes (called from Opus bridge). */
        build_opus_packet(opusData, seq, timestamp) {
            const u8 = opusData instanceof Uint8Array ? opusData : new Uint8Array(opusData);
            const ptr = passArray8(u8, w().__wbindgen_malloc);
            const ret = w().solunatx_build_opus_packet(this._ptr, ptr, WASM_VECTOR_LEN, seq, timestamp);
            const pkt = getArrayU8(ret[0], ret[1]).slice();
            w().__wbindgen_free(ret[0], ret[1], 1);
            return pkt;
        }
        set_opus_enabled(en)     { w().solunatx_set_opus_enabled(this._ptr, en); }
        opus_enabled()           { return w().solunatx_opus_enabled(this._ptr) !== 0; }
        free() { w().__wbg_solunatx_free(this._ptr, 0); this._ptr = 0; }
    }

    // ── High-level SolunaAudio API ─────────────────────────

    class SolunaAudio {
        constructor() {
            this.player = null;
            this.ctx = null;
            this.worklet = null;
            this.gainNode = null;
            this.analyser = null;
            this.ws = null;
            this.active = false;
            this.pktCount = 0;
            this.underruns = 0;
            this.opusEnabled = false;
            this.onStats = null;
            this.onStateChange = null;
            // Clock sync state (NTP-like over WebSocket)
            this._syncTimer = null;
            this._syncPingCount = 0;
            this._clockOffsetMs = 0;
        }

        async init(outChannels, sampleRate) {
            outChannels = outChannels || 2;
            sampleRate = sampleRate || 48000;

            await loadWasm();
            this.player = new SolunaPlayer(outChannels);

            // Initialize Opus decoder bridge if available
            if (globalThis.SolunaOpusBridge && globalThis.SolunaOpusBridge.isSupported()) {
                var opusOk = await globalThis.SolunaOpusBridge.initDecoder(this.player);
                if (opusOk) {
                    this.opusEnabled = true;
                    console.log('[SolunaAudio] Opus decoding enabled via WebCodecs');
                }
            }

            this.ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: sampleRate });
            if (this.ctx.state === 'suspended') await this.ctx.resume().catch(function(){});

            // Load AudioWorklet
            await this.ctx.audioWorklet.addModule(WORKLET_URL);

            this.worklet = new AudioWorkletNode(this.ctx, 'soluna-wasm', { outputChannelCount: [2] });
            this.gainNode = this.ctx.createGain();
            this.analyser = this.ctx.createAnalyser();
            this.analyser.fftSize = 2048;
            this.analyser.smoothingTimeConstant = 0.7;

            this.worklet.connect(this.gainNode);
            this.gainNode.connect(this.analyser);
            this.analyser.connect(this.ctx.destination);

            var self = this;
            this.worklet.port.onmessage = function(e) {
                var data = e.data;
                if (data.type === 'underrun') self.underruns++;
                if (data.type === 'stats' && self.onStats) {
                    self.onStats({
                        fill: data.fill,
                        packets: self.pktCount,
                        underruns: self.underruns,
                        txChannels: self.player ? self.player.detected_tx_channels() : 0,
                        syncLocked: self.player ? self.player.sync_locked() : false,
                        syncDelayMs: self.player ? self.player.sync_delay_ms() : 0,
                        targetFill: self.player ? self.player.target_fill_frames() : 0,
                        netDelayMs: self.player ? self.player.net_delay_ms() : 0,
                        clockOffsetMs: self._clockOffsetMs || 0,
                        opusPackets: self.player ? self.player.opus_packet_count() : 0,
                        opusDecoded: self.player ? self.player.opus_decoded_count() : 0,
                        opusEnabled: self.opusEnabled,
                    });
                }
            };

            this.active = true;
        }

        connect(url) {
            if (this.ws) this.disconnect();
            this._wsUrl = url;
            this._reconnectDelay = 1000;
            this._reconnectTimer = null;
            this._intentionalClose = false;
            this._openWs(url);
        }

        _openWs(url) {
            var self = this;
            try {
                this.ws = new WebSocket(url);
            } catch (e) {
                console.warn('[SolunaAudio] WebSocket creation failed:', e.message);
                self._scheduleReconnect();
                return;
            }
            this.ws.binaryType = 'arraybuffer';
            this.ws.onopen = function() {
                self._reconnectDelay = 1000; // reset backoff on success
                if (self.onStateChange) self.onStateChange('connected');
                // Start periodic clock sync (NTP-like, every 5 seconds)
                self._startClockSync();
            };
            this.ws.onmessage = function(evt) {
                if (evt.data instanceof ArrayBuffer) self._handlePacket(new Uint8Array(evt.data));
            };
            this.ws.onclose = function() {
                if (self._intentionalClose) {
                    if (self.onStateChange) self.onStateChange('disconnected');
                    return;
                }
                self._scheduleReconnect();
            };
            this.ws.onerror = function() {
                // onerror is always followed by onclose, so reconnect is handled there
            };
        }

        _scheduleReconnect() {
            var self = this;
            if (self._intentionalClose || !self._wsUrl) return;
            if (self.onStateChange) self.onStateChange('reconnecting');
            console.log('[SolunaAudio] reconnecting in ' + self._reconnectDelay + 'ms');
            self._reconnectTimer = setTimeout(function() {
                self._reconnectTimer = null;
                if (!self._intentionalClose && self._wsUrl) {
                    self._openWs(self._wsUrl);
                }
            }, self._reconnectDelay);
            // Exponential backoff: 1s → 2s → 4s → 8s → … → 30s max
            self._reconnectDelay = Math.min(self._reconnectDelay * 2, 30000);
        }

        _handlePacket(data) {
            if (!this.player || !this.active) return;

            // Clock sync pong: PT=125 (0x7D marker), exactly 25 bytes
            if (data.length === 25 && data[0] === 0x7D) {
                this._handleSyncPong(data);
                return;
            }

            this.player.push_packet(data);
            this.pktCount++;

            var avail = this.player.available();
            if (avail >= 128) {
                var frames = Math.min(avail, 960);
                var l = new Float32Array(frames);
                var r = new Float32Array(frames);
                var pulled = this.player.pull_audio(l, r);
                if (pulled > 0 && this.worklet) {
                    var sl = l.subarray(0, pulled);
                    var sr = r.subarray(0, pulled);
                    this.worklet.port.postMessage(
                        { type: 'audio', l: sl, r: sr },
                        [sl.buffer, sr.buffer]
                    );
                }
            }
        }

        pushPacket(data) {
            var u8 = data instanceof Uint8Array ? data : new Uint8Array(data);
            this._handlePacket(u8);
        }

        disconnect() {
            this._intentionalClose = true;
            this._stopClockSync();
            if (this._reconnectTimer) { clearTimeout(this._reconnectTimer); this._reconnectTimer = null; }
            if (this.ws) { this.ws.close(); this.ws = null; }
            this._wsUrl = null;
        }

        setVolume(v) { if (this.gainNode) this.gainNode.gain.value = v; }
        setOutputChannels(ch) { if (this.player) this.player.set_output_channels(ch); }
        setSyncEnabled(en) { if (this.player) this.player.set_sync_enabled(en); }
        setSyncDelay(ms) { if (this.player) this.player.set_sync_delay_ms(ms); }
        getAnalyser() { return this.analyser; }
        getClockOffsetMs() { return this._clockOffsetMs; }

        // ── Clock sync (NTP-like over WebSocket, PT=125) ──────────────────
        // Sends a 25-byte binary sync ping every 5 seconds.
        // Relay stamps T2/T3 and returns it. We compute offset from T1-T4.

        _startClockSync() {
            this._stopClockSync();
            this._syncPingCount = 0;
            var self = this;
            // Send first ping immediately, then every 5s
            self._sendSyncPing();
            self._syncTimer = setInterval(function() { self._sendSyncPing(); }, 5000);
        }

        _stopClockSync() {
            if (this._syncTimer) { clearInterval(this._syncTimer); this._syncTimer = null; }
        }

        _sendSyncPing() {
            if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;

            // Build 25-byte sync packet: [0x7D] [T1: 8 bytes LE] [T2: 8 bytes zero] [T3: 8 bytes zero]
            var buf = new ArrayBuffer(25);
            var view = new DataView(buf);
            view.setUint8(0, 0x7D); // PT=125 sync marker

            // T1 = Date.now() converted to nanoseconds (64-bit LE)
            // JS Date.now() is ms since epoch. Convert to ns.
            var now_ms = Date.now();
            var now_ns_hi = Math.floor(now_ms * 1e6 / 4294967296); // upper 32 bits
            var now_ns_lo = (now_ms * 1e6) >>> 0;                   // lower 32 bits
            view.setUint32(1, now_ns_lo, true);  // little-endian
            view.setUint32(5, now_ns_hi, true);
            // T2 and T3 are zeros (relay will fill them)

            this.ws.send(buf);
        }

        _handleSyncPong(data) {
            // T4 = local receive time in nanoseconds
            var t4_ms = Date.now();
            var t4_ns = t4_ms * 1e6;

            // Extract T1, T2, T3 (64-bit LE nanoseconds)
            var dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
            var t1_lo = dv.getUint32(1, true);
            var t1_hi = dv.getUint32(5, true);
            var t1_ns = t1_hi * 4294967296 + t1_lo;

            var t2_lo = dv.getUint32(9, true);
            var t2_hi = dv.getUint32(13, true);
            var t2_ns = t2_hi * 4294967296 + t2_lo;

            var t3_lo = dv.getUint32(17, true);
            var t3_hi = dv.getUint32(21, true);
            var t3_ns = t3_hi * 4294967296 + t3_lo;

            // Validate: T2 and T3 must be non-zero
            if (t2_ns === 0 || t3_ns === 0) return;

            // NTP offset = ((T2-T1) + (T3-T4)) / 2  (in nanoseconds)
            var offset_ns = ((t2_ns - t1_ns) + (t3_ns - t4_ns)) / 2;
            var rtt_ns = (t4_ns - t1_ns) - (t3_ns - t2_ns);

            // Reject outliers: RTT > 500ms
            if (rtt_ns < 0 || rtt_ns > 500e6) return;

            var offset_ms = offset_ns / 1e6;
            var rtt_ms = rtt_ns / 1e6;

            // EMA smoothing
            this._syncPingCount++;
            if (this._syncPingCount === 1) {
                this._clockOffsetMs = offset_ms;
            } else {
                var alpha = this._syncPingCount < 5 ? 0.3 : 0.1;
                this._clockOffsetMs = this._clockOffsetMs * (1 - alpha) + offset_ms * alpha;
            }

            // Pass to WASM player
            if (this.player) {
                this.player.set_clock_offset(this._clockOffsetMs);
            }

            if (this._syncPingCount <= 3) {
                console.log('[clock-sync] offset=' + offset_ms.toFixed(2) + 'ms rtt=' + rtt_ms.toFixed(2) + 'ms #' + this._syncPingCount);
            }
        }

        destroy() {
            this.disconnect();
            this.active = false;
            if (globalThis.SolunaOpusBridge) globalThis.SolunaOpusBridge.destroy();
            if (this.worklet) { this.worklet.disconnect(); this.worklet = null; }
            if (this.gainNode) { this.gainNode.disconnect(); this.gainNode = null; }
            if (this.analyser) { this.analyser.disconnect(); this.analyser = null; }
            if (this.ctx) { this.ctx.close().catch(function(){}); this.ctx = null; }
            if (this.player) { this.player.free(); this.player = null; }
        }
    }

    // Expose globally
    window.SolunaAudio = SolunaAudio;
    window.SolunaTxEncoder = SolunaTxEncoder;
})();
