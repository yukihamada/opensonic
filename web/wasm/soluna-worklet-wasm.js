/**
 * Soluna AudioWorklet Processor — WASM-in-Worklet mode
 *
 * Runs the entire WASM audio engine inside the AudioWorkletGlobalScope,
 * eliminating main-thread-to-audio-thread data transfer.
 *
 * Protocol (Main thread -> Worklet):
 *   { type: 'init', wasmBytes: ArrayBuffer }          — one-time WASM load
 *   { type: 'packet', data: ArrayBuffer }              — raw OSTP packet from WebSocket
 *   { type: 'config', key: string, value: any }        — runtime config (volume, sync, etc.)
 *
 * Protocol (Worklet -> Main thread):
 *   { type: 'ready' }                                  — WASM loaded successfully
 *   { type: 'error', message: string }                 — WASM load or runtime error
 *   { type: 'underrun' }                               — buffer underrun
 *   { type: 'stats', ... }                             — periodic stats (~1/s)
 *   { type: 'clock_sync', data: ArrayBuffer }          — PT=125 clock sync forwarded to main
 */
class SolunaWasmInWorkletProcessor extends AudioWorkletProcessor {
    constructor() {
        super();

        // WASM state
        this._wasm = null;       // WebAssembly.Instance.exports
        this._memory = null;     // WebAssembly.Memory
        this._playerPtr = 0;     // Pointer to SolunaPlayer struct
        this._ready = false;

        // Cached typed array views (invalidated when memory grows)
        this._cachedU8 = null;
        this._cachedF32 = null;

        // Ring buffer for incoming packets (lock-free SPSC)
        // Packets arrive via postMessage, consumed in process()
        this._packetQueue = [];
        this._maxQueueLen = 512;

        // Stats
        this._tick = 0;
        this._pktCount = 0;

        // Output buffers (reused per process() call)
        this._outL = new Float32Array(128);
        this._outR = new Float32Array(128);

        this.port.onmessage = (e) => this._onMessage(e.data);
    }

    _onMessage(msg) {
        switch (msg.type) {
            case 'init':
                this._initWasm(msg.wasmBytes, msg.outChannels || 2);
                break;
            case 'packet':
                if (this._packetQueue.length < this._maxQueueLen) {
                    this._packetQueue.push(new Uint8Array(msg.data));
                }
                break;
            case 'config':
                this._applyConfig(msg.key, msg.value);
                break;
        }
    }

    _getU8() {
        if (!this._cachedU8 || this._cachedU8.byteLength === 0) {
            this._cachedU8 = new Uint8Array(this._memory.buffer);
        }
        return this._cachedU8;
    }

    _getF32() {
        if (!this._cachedF32 || this._cachedF32.byteLength === 0) {
            this._cachedF32 = new Float32Array(this._memory.buffer);
        }
        return this._cachedF32;
    }

    _invalidateViews() {
        this._cachedU8 = null;
        this._cachedF32 = null;
    }

    _passArray8(arr) {
        var ptr = this._wasm.__wbindgen_malloc(arr.length, 1) >>> 0;
        this._getU8().set(arr, ptr);
        return { ptr: ptr, len: arr.length };
    }

    _passArrayF32(arr) {
        var ptr = this._wasm.__wbindgen_malloc(arr.length * 4, 4) >>> 0;
        this._getF32().set(arr, ptr / 4);
        return { ptr: ptr, len: arr.length };
    }

    async _initWasm(wasmBytes, outChannels) {
        try {
            var self = this;

            // Build imports that the WASM module expects.
            // In worklet context, we provide shims for all JS callbacks.
            // Opus bridge is NOT available (no WebCodecs in worklet),
            // so on_opus_packet / on_opus_encode are no-ops.
            // The WASM will fall back to silence generation for Opus packets.
            var imports = {
                __proto__: null,
                './soluna_wasm_bg.js': {
                    __proto__: null,
                    __wbg___wbindgen_copy_to_typed_array_d2f20acdab8e0740: function(arg0, arg1, arg2) {
                        new Uint8Array(arg2.buffer, arg2.byteOffset, arg2.byteLength).set(
                            self._getU8().subarray(arg0 >>> 0, (arg0 >>> 0) + arg1)
                        );
                    },
                    __wbg___wbindgen_throw_6ddd609b62940d55: function(arg0, arg1) {
                        // In worklet, we cannot throw usefully. Post error to main thread.
                        var bytes = self._getU8().subarray(arg0 >>> 0, (arg0 >>> 0) + arg1);
                        var msg = '';
                        for (var i = 0; i < bytes.length; i++) msg += String.fromCharCode(bytes[i]);
                        self.port.postMessage({ type: 'error', message: msg });
                    },
                    __wbg_now_16f0c993d5dd6c27: function() {
                        // performance.now() is NOT wall-clock; Date.now() is not available
                        // in some worklet contexts. Use currentTime * 1000 as fallback.
                        // Note: Date is usually available in AudioWorkletGlobalScope.
                        if (typeof Date !== 'undefined') return Date.now();
                        return currentTime * 1000;
                    },
                    // Opus bridge callbacks: no-ops in worklet (no WebCodecs).
                    // WASM will see has_opus_bridge()=false and generate silence for Opus packets.
                    __wbg_on_opus_packet_3cd0a052193e55de: function() {
                        // no-op: Opus decode unavailable in AudioWorklet
                    },
                    __wbg_on_opus_encode_016dc3431f7bf0b5: function() {
                        // no-op: Opus encode unavailable in AudioWorklet
                    },
                    __wbindgen_init_externref_table: function() {
                        var table = self._wasm.__wbindgen_externrefs;
                        var offset = table.grow(4);
                        table.set(0, undefined);
                        table.set(offset + 0, undefined);
                        table.set(offset + 1, null);
                        table.set(offset + 2, true);
                        table.set(offset + 3, false);
                    },
                }
            };

            var result = await WebAssembly.instantiate(wasmBytes, imports);
            this._wasm = result.instance.exports;
            this._memory = this._wasm.memory;
            this._invalidateViews();

            // Run wasm-bindgen initialization
            this._wasm.__wbindgen_start();

            // Do NOT set opus bridge available — it is not available in worklet.
            // WASM defaults to has_opus_bridge()=false, which triggers silence fallback for Opus.

            // Create SolunaPlayer
            this._playerPtr = this._wasm.solunaplayer_new(outChannels) >>> 0;
            this._ready = true;

            this.port.postMessage({ type: 'ready' });
        } catch (err) {
            this.port.postMessage({ type: 'error', message: 'WASM init failed: ' + (err.message || err) });
        }
    }

    _applyConfig(key, value) {
        if (!this._ready) return;
        switch (key) {
            case 'outputChannels':
                this._wasm.solunaplayer_set_output_channels(this._playerPtr, value);
                break;
            case 'syncEnabled':
                this._wasm.solunaplayer_set_sync_enabled(this._playerPtr, value);
                break;
            case 'syncDelayMs':
                this._wasm.solunaplayer_set_sync_delay_ms(this._playerPtr, value);
                break;
            case 'clockOffset':
                this._wasm.solunaplayer_set_clock_offset(this._playerPtr, value);
                break;
            case 'talkMode':
                this._wasm.solunaplayer_set_talk_mode(this._playerPtr, value);
                break;
            case 'clear':
                this._wasm.solunaplayer_clear(this._playerPtr);
                break;
        }
    }

    _drainPackets() {
        var queue = this._packetQueue;
        for (var i = 0; i < queue.length; i++) {
            var pkt = queue[i];

            // Clock sync packets (PT=125, 25 bytes): forward to main thread for NTP
            if (pkt.length === 25 && pkt[0] === 0x7D) {
                this.port.postMessage(
                    { type: 'clock_sync', data: pkt.buffer.slice(0) }
                );
                continue;
            }

            // Push packet into WASM player
            var ref = this._passArray8(pkt);
            this._wasm.solunaplayer_push_packet(this._playerPtr, ref.ptr, ref.len);
            this._pktCount++;
        }
        queue.length = 0;
    }

    process(inputs, outputs) {
        if (!this._ready) return true;

        // Drain any queued packets into WASM
        if (this._packetQueue.length > 0) {
            this._drainPackets();
        }

        var outL = outputs[0][0];
        var outR = outputs[0][1] || outputs[0][0];
        var n = outL.length; // typically 128

        // Ensure temp buffers are the right size
        if (this._outL.length !== n) {
            this._outL = new Float32Array(n);
            this._outR = new Float32Array(n);
        }

        // Pull audio directly from WASM
        var refL = this._passArrayF32(this._outL);
        var refR = this._passArrayF32(this._outR);

        var pulled = this._wasm.solunaplayer_pull_audio(
            this._playerPtr,
            refL.ptr, refL.len, this._outL,
            refR.ptr, refR.len, this._outR
        ) >>> 0;

        if (pulled === 0) {
            outL.fill(0);
            outR.fill(0);
            this.port.postMessage({ type: 'underrun' });
        } else {
            // Copy from temp buffers (WASM writes back via __wbindgen_copy_to_typed_array)
            outL.set(this._outL);
            if (outR !== outL) outR.set(this._outR);
        }

        // Stats every ~1s (48000 / 128 = 375)
        if (++this._tick >= 375) {
            this._tick = 0;
            var w = this._wasm;
            var p = this._playerPtr;
            this.port.postMessage({
                type: 'stats',
                fill: w.solunaplayer_available(p) >>> 0,
                packets: this._pktCount,
                underruns: w.solunaplayer_underrun_count(p) >>> 0,
                txChannels: w.solunaplayer_detected_tx_channels(p) >>> 0,
                syncLocked: w.solunaplayer_sync_locked(p) !== 0,
                syncDelayMs: w.solunaplayer_sync_delay_ms(p) >>> 0,
                targetFill: w.solunaplayer_target_fill_frames(p) >>> 0,
                netDelayMs: w.solunaplayer_net_delay_ms(p),
                clockOffsetMs: w.solunaplayer_clock_offset_ms(p),
                opusPackets: w.solunaplayer_opus_packet_count(p) >>> 0,
                opusDecoded: w.solunaplayer_opus_decoded_count(p) >>> 0,
            });
        }

        return true;
    }
}

registerProcessor('soluna-wasm-in-worklet', SolunaWasmInWorkletProcessor);
