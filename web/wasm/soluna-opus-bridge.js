/**
 * Soluna Opus Bridge — WebCodecs-based Opus encode/decode for WASM.
 *
 * Bridges the gap between WASM (which cannot include native Opus libs)
 * and the browser's built-in Opus codec via the WebCodecs API.
 *
 * Usage:
 *   <script src="/wasm/soluna-opus-bridge.js"></script>
 *   <script src="/wasm/soluna-audio.js"></script>
 *   // Bridge auto-registers on globalThis.SolunaOpusBridge
 *   // WASM calls bridge functions when PT_OPUS packets arrive
 *
 * Requirements:
 *   - Browser with WebCodecs API (Chrome 94+, Edge 94+, Safari 16.4+)
 *   - AudioDecoder and AudioEncoder support for 'opus' codec
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
    const SAMPLE_RATE = 48000;
    const CHANNELS = 2;       // Opus is typically stereo

    // ── Feature detection ──────────────────────────────────

    function isSupported() {
        return typeof AudioDecoder !== 'undefined' && typeof AudioEncoder !== 'undefined';
    }

    // ── Decoder (RX path: Opus bytes → f32 PCM → WASM ring buffer) ──

    async function initDecoder(player) {
        if (!isSupported()) {
            console.warn('[SolunaOpusBridge] WebCodecs not available — Opus decode disabled');
            return false;
        }

        _player = player;

        // Check if opus decoding is supported
        try {
            const support = await AudioDecoder.isConfigSupported({
                codec: 'opus',
                sampleRate: SAMPLE_RATE,
                numberOfChannels: CHANNELS,
            });
            if (!support.supported) {
                console.warn('[SolunaOpusBridge] Opus decoding not supported by this browser');
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
                console.error('[SolunaOpusBridge] Decoder error:', e.message);
            }
        });

        _decoder.configure({
            codec: 'opus',
            sampleRate: SAMPLE_RATE,
            numberOfChannels: CHANNELS,
        });

        _decoderReady = true;
        console.log('[SolunaOpusBridge] Opus decoder initialized (WebCodecs)');
        return true;
    }

    // ── Encoder (TX path: f32 PCM → Opus bytes → OSTP packet → WebSocket) ──

    async function initEncoder(tx, ws) {
        if (!isSupported()) {
            console.warn('[SolunaOpusBridge] WebCodecs not available — Opus encode disabled');
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
     * Feeds raw Opus bytes into WebCodecs AudioDecoder.
     */
    function on_opus_packet(data, seq, timestamp, channels) {
        if (!_decoder || !_decoderReady) return;

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
    };

    console.log('[SolunaOpusBridge] Loaded. WebCodecs supported:', isSupported());
})();
