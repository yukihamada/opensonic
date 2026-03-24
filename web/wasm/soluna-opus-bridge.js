/**
 * Soluna Opus Bridge — WebCodecs-based Opus encode/decode for WASM,
 * with WASM libopus fallback for browsers without WebCodecs support.
 *
 * Decode priority:
 *   1. WebCodecs AudioDecoder (Chrome 94+, Edge 94+, Safari 16.4+)
 *   2. WASM libopus decoder (any browser with WebAssembly — requires opus-decoder.wasm)
 *   3. Silence generation (handled by WASM core when no decoder is available)
 *
 * Usage:
 *   <script src="/wasm/soluna-opus-wasm.js"></script>  <!-- WASM fallback, load first -->
 *   <script src="/wasm/soluna-opus-bridge.js"></script>
 *   <script src="/wasm/soluna-audio.js"></script>
 *   // Bridge auto-registers on globalThis.SolunaOpusBridge
 *   // WASM calls bridge functions when PT_OPUS packets arrive
 *
 * Requirements for WebCodecs path:
 *   - Browser with WebCodecs API (Chrome 94+, Edge 94+, Safari 16.4+)
 *   - AudioDecoder and AudioEncoder support for 'opus' codec
 *
 * Requirements for WASM fallback path:
 *   - Browser with WebAssembly support (all modern browsers)
 *   - opus-decoder.wasm served at /wasm/opus-wasm/opus-decoder.wasm
 *   - Build with: bash web/wasm/build-opus-wasm.sh
 */
(function() {
    'use strict';

    // ── State ──────────────────────────────────────────────

    let _player = null;       // SolunaPlayer WASM instance (set via init())
    let _tx = null;           // SolunaTx WASM instance (set via initTx())
    let _ws = null;           // WebSocket for sending encoded packets
    let _decoder = null;      // WebCodecs AudioDecoder
    let _encoder = null;      // WebCodecs AudioEncoder
    let _decoderReady = false;
    let _encoderReady = false;
    let _wasmFallback = false; // true when using WASM libopus instead of WebCodecs
    const SAMPLE_RATE = 48000;
    const CHANNELS = 2;       // Opus is typically stereo

    // ── Feature detection ──────────────────────────────────

    /** Returns true if WebCodecs AudioDecoder/AudioEncoder are available. */
    function isWebCodecsSupported() {
        return typeof AudioDecoder !== 'undefined' && typeof AudioEncoder !== 'undefined';
    }

    /** Returns true if the WASM libopus fallback is loaded and available. */
    function isWasmFallbackLoaded() {
        return !!(globalThis.SolunaOpusWasm);
    }

    /**
     * Returns true if any Opus decode path is available (WebCodecs or WASM fallback).
     * This is the function checked by soluna-audio.js to decide whether to enable
     * the opus bridge in WASM core.
     */
    function isSupported() {
        return isWebCodecsSupported() || isWasmFallbackLoaded();
    }

    // ── WASM Fallback Decoder ──────────────────────────────

    /**
     * Try to initialize the WASM libopus fallback decoder.
     * Called when WebCodecs is not available.
     * Returns true if WASM fallback is ready.
     */
    async function initWasmFallback(player) {
        if (!isWasmFallbackLoaded()) {
            console.warn('[SolunaOpusBridge] SolunaOpusWasm not loaded — WASM fallback unavailable');
            return false;
        }

        _player = player;

        try {
            var ok = await globalThis.SolunaOpusWasm.init(CHANNELS);
            if (!ok) {
                console.warn('[SolunaOpusBridge] WASM libopus init failed');
                return false;
            }

            _wasmFallback = true;
            _decoderReady = true;
            console.log('[SolunaOpusBridge] Opus decoder initialized (WASM libopus fallback)');
            return true;
        } catch (e) {
            console.warn('[SolunaOpusBridge] WASM fallback init error:', e.message);
            return false;
        }
    }

    /**
     * Decode an Opus packet using the WASM fallback and push to player.
     */
    function decodeViaWasmFallback(data, channels) {
        if (!globalThis.SolunaOpusWasm || !globalThis.SolunaOpusWasm.isAvailable()) return;
        if (!_player) return;

        // Determine frame size from Opus TOC byte
        var frameSize = estimateFrameSize(data);

        var pcm = globalThis.SolunaOpusWasm.decode(data, frameSize);
        if (pcm && pcm.length > 0) {
            var ch = globalThis.SolunaOpusWasm.getChannels();
            _player.push_decoded_opus(pcm, ch);
        }
    }

    /**
     * Estimate Opus frame size in samples from the TOC byte.
     * Matches the logic in soluna-core/src/opus.rs parse_packet_info().
     */
    function estimateFrameSize(data) {
        if (!data || data.length === 0) return 960; // default 20ms

        var tocByte = data[0];
        var config = (tocByte >> 3) & 0x1F;
        var frameCountCode = tocByte & 0x03;

        // Frame duration in microseconds (from RFC 6716 Table 2)
        var durations = [
            10000, 20000, 40000, 60000, // SILK NB
            10000, 20000, 40000, 60000, // SILK MB
            10000, 20000, 40000, 60000, // SILK WB
            10000, 20000,               // Hybrid SWB
            10000, 20000,               // Hybrid FB
            2500,  5000,  10000, 20000, // CELT NB
            2500,  5000,  10000, 20000, // CELT MB
            2500,  5000,  10000, 20000, // CELT WB
            2500,  5000,  10000, 20000, // CELT SWB
        ];

        var frameDurationUs = durations[config] || 20000;

        var frameCount;
        switch (frameCountCode) {
            case 0: frameCount = 1; break;
            case 1: case 2: frameCount = 2; break;
            case 3:
                frameCount = (data.length >= 2) ? (data[1] & 0x3F) : 1;
                break;
            default: frameCount = 1;
        }

        // Total samples per channel at 48kHz
        var totalDurationUs = frameDurationUs * frameCount;
        var samplesPerChannel = Math.floor(totalDurationUs * 48 / 1000);

        return samplesPerChannel;
    }

    // ── Decoder (RX path: Opus bytes → f32 PCM → WASM ring buffer) ──

    async function initDecoder(player) {
        // Try WebCodecs first
        if (isWebCodecsSupported()) {
            var webCodecsOk = await initWebCodecsDecoder(player);
            if (webCodecsOk) return true;
            // WebCodecs available but Opus not supported — try WASM fallback
        }

        // Fall back to WASM libopus
        return await initWasmFallback(player);
    }

    /**
     * Initialize WebCodecs AudioDecoder for Opus.
     * Separated from initDecoder to allow fallback logic.
     */
    async function initWebCodecsDecoder(player) {
        _player = player;

        // Check if opus decoding is supported
        try {
            const support = await AudioDecoder.isConfigSupported({
                codec: 'opus',
                sampleRate: SAMPLE_RATE,
                numberOfChannels: CHANNELS,
            });
            if (!support.supported) {
                console.warn('[SolunaOpusBridge] Opus decoding not supported by WebCodecs');
                return false;
            }
        } catch (e) {
            console.warn('[SolunaOpusBridge] AudioDecoder.isConfigSupported failed:', e);
            return false;
        }

        _decoder = new AudioDecoder({
            output: function(audioData) {
                // AudioData → f32 interleaved → push to WASM player
                if (!_player) { audioData.close(); return; }

                const frames = audioData.numberOfFrames;
                const ch = audioData.numberOfChannels;
                const pcm = new Float32Array(frames * ch);

                // Copy each channel's data (AudioData stores planar by default)
                if (ch === 1) {
                    audioData.copyTo(pcm, { planeIndex: 0, format: 'f32-planar' });
                } else {
                    // Interleave L and R
                    const tmpL = new Float32Array(frames);
                    const tmpR = new Float32Array(frames);
                    audioData.copyTo(tmpL, { planeIndex: 0, format: 'f32-planar' });
                    audioData.copyTo(tmpR, { planeIndex: 1, format: 'f32-planar' });
                    for (let i = 0; i < frames; i++) {
                        pcm[i * 2]     = tmpL[i];
                        pcm[i * 2 + 1] = tmpR[i];
                    }
                }

                audioData.close();

                // Feed decoded PCM back into WASM ring buffer
                _player.push_decoded_opus(pcm, ch);
            },
            error: function(e) {
                console.error('[SolunaOpusBridge] WebCodecs decoder error:', e.message);
            }
        });

        _decoder.configure({
            codec: 'opus',
            sampleRate: SAMPLE_RATE,
            numberOfChannels: CHANNELS,
        });

        _decoderReady = true;
        _wasmFallback = false;
        console.log('[SolunaOpusBridge] Opus decoder initialized (WebCodecs)');
        return true;
    }

    // ── Encoder (TX path: f32 PCM → Opus bytes → OSTP packet → WebSocket) ──
    // Note: Encoding still requires WebCodecs — WASM fallback is decode-only.

    async function initEncoder(tx, ws) {
        if (!isWebCodecsSupported()) {
            console.warn('[SolunaOpusBridge] WebCodecs not available — Opus encode disabled');
            console.warn('[SolunaOpusBridge] (WASM fallback supports decode only)');
            return false;
        }

        _tx = tx;
        _ws = ws;

        try {
            const support = await AudioEncoder.isConfigSupported({
                codec: 'opus',
                sampleRate: SAMPLE_RATE,
                numberOfChannels: CHANNELS,
                bitrate: 128000,
            });
            if (!support.supported) {
                console.warn('[SolunaOpusBridge] Opus encoding not supported by this browser');
                return false;
            }
        } catch (e) {
            console.warn('[SolunaOpusBridge] AudioEncoder.isConfigSupported failed:', e);
            return false;
        }

        _encoder = new AudioEncoder({
            output: function(chunk, metadata) {
                // EncodedAudioChunk → raw bytes → build OSTP packet → send
                if (!_tx || !_ws || _ws.readyState !== WebSocket.OPEN) return;

                const opusData = new Uint8Array(chunk.byteLength);
                chunk.copyTo(opusData);

                // Retrieve seq/timestamp from the pending queue
                var pending = _encodePending.shift();
                if (!pending) return;

                var pkt = _tx.build_opus_packet(opusData, pending.seq, pending.timestamp);
                if (pkt && pkt.length > 0) {
                    _ws.send(pkt);
                }
            },
            error: function(e) {
                console.error('[SolunaOpusBridge] Encoder error:', e.message);
            }
        });

        _encoder.configure({
            codec: 'opus',
            sampleRate: SAMPLE_RATE,
            numberOfChannels: CHANNELS,
            bitrate: 128000,
            opus: {
                frameDuration: 20000, // 20ms frames
            }
        });

        _encoderReady = true;
        console.log('[SolunaOpusBridge] Opus encoder initialized (WebCodecs)');
        return true;
    }

    // Queue for matching encode output to OSTP sequence/timestamp
    var _encodePending = [];

    // ── Bridge functions called from WASM ──────────────────

    /**
     * Called by WASM when a PT_OPUS packet arrives.
     * Routes to WebCodecs AudioDecoder or WASM fallback.
     */
    function on_opus_packet(data, seq, timestamp, channels) {
        if (!_decoderReady) return;

        // ── WASM fallback path (synchronous decode) ──
        if (_wasmFallback) {
            // data may be a subarray view — copy it to avoid referencing freed WASM memory
            var dataCopy = new Uint8Array(data.length);
            dataCopy.set(data);
            decodeViaWasmFallback(dataCopy, channels);
            return;
        }

        // ── WebCodecs path (async decode) ──
        if (!_decoder) return;

        try {
            var chunk = new EncodedAudioChunk({
                type: 'key', // Opus frames are always independently decodable
                timestamp: timestamp * (1000000 / SAMPLE_RATE), // Convert sample offset to microseconds
                data: data,
            });
            _decoder.decode(chunk);
        } catch (e) {
            console.warn('[SolunaOpusBridge] Decode error:', e.message);
        }
    }

    /**
     * Called by WASM when SolunaTx wants to encode PCM as Opus.
     * Feeds f32 PCM into WebCodecs AudioEncoder.
     * Note: WASM fallback does not support encoding.
     */
    function on_opus_encode(pcm, channels, sampleRate, seq, timestamp, ssrc, streamId) {
        if (!_encoder || !_encoderReady) return;

        try {
            // Create AudioData from f32 interleaved PCM
            var frames = Math.floor(pcm.length / channels);

            // AudioData expects planar format
            var audioData;
            if (channels === 1) {
                audioData = new AudioData({
                    format: 'f32-planar',
                    sampleRate: sampleRate,
                    numberOfFrames: frames,
                    numberOfChannels: 1,
                    timestamp: timestamp * (1000000 / sampleRate),
                    data: pcm,
                });
            } else {
                // De-interleave to planar
                var planar = new Float32Array(pcm.length);
                for (var i = 0; i < frames; i++) {
                    planar[i] = pcm[i * channels];             // L
                    planar[frames + i] = pcm[i * channels + 1]; // R
                }
                audioData = new AudioData({
                    format: 'f32-planar',
                    sampleRate: sampleRate,
                    numberOfFrames: frames,
                    numberOfChannels: channels,
                    timestamp: timestamp * (1000000 / sampleRate),
                    data: planar,
                });
            }

            _encodePending.push({ seq: seq, timestamp: timestamp });
            _encoder.encode(audioData);
            audioData.close();
        } catch (e) {
            console.warn('[SolunaOpusBridge] Encode error:', e.message);
        }
    }

    // ── Public API ─────────────────────────────────────────

    function destroy() {
        if (_decoder) {
            try { _decoder.close(); } catch(e) {}
            _decoder = null;
            _decoderReady = false;
        }
        if (_encoder) {
            try { _encoder.close(); } catch(e) {}
            _encoder = null;
            _encoderReady = false;
        }
        // Destroy WASM fallback decoder
        if (_wasmFallback && globalThis.SolunaOpusWasm) {
            try { globalThis.SolunaOpusWasm.destroy(); } catch(e) {}
        }
        _wasmFallback = false;
        _player = null;
        _tx = null;
        _ws = null;
        _encodePending = [];
    }

    // Register the bridge globally so WASM can call it
    globalThis.SolunaOpusBridge = {
        // Called from WASM via wasm-bindgen extern
        on_opus_packet: on_opus_packet,
        on_opus_encode: on_opus_encode,

        // Called from JS application code
        initDecoder: initDecoder,
        initEncoder: initEncoder,
        destroy: destroy,
        isSupported: isSupported,

        // Status
        get decoderReady() { return _decoderReady; },
        get encoderReady() { return _encoderReady; },
        /** Whether the decoder is using the WASM libopus fallback (true) or WebCodecs (false). */
        get wasmFallback() { return _wasmFallback; },
    };

    console.log('[SolunaOpusBridge] Loaded. WebCodecs:', isWebCodecsSupported(),
                '| WASM fallback:', isWasmFallbackLoaded());
})();
