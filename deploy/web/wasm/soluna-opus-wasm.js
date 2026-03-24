/**
 * Soluna Opus WASM Decoder — fallback for browsers without WebCodecs.
 *
 * Loads a pre-compiled libopus.wasm (built from Xiph's C libopus via Emscripten)
 * and exposes a simple decode API that mirrors what WebCodecs AudioDecoder provides.
 *
 * Build the WASM module:
 *   bash web/wasm/build-opus-wasm.sh
 *
 * This produces:
 *   web/wasm/opus-wasm/opus-decoder.wasm  (~120KB gzipped)
 *   web/wasm/opus-wasm/opus-decoder.js    (Emscripten glue, loaded by this file)
 *
 * If the pre-compiled WASM is not available, this module is a no-op
 * and isAvailable() returns false.
 *
 * Usage:
 *   <script src="/wasm/soluna-opus-wasm.js"></script>
 *   // Auto-registers on globalThis.SolunaOpusWasm
 *   // Called by SolunaOpusBridge as a fallback decoder
 */
(function() {
    'use strict';

    // ── Constants ──────────────────────────────────────────
    const OPUS_WASM_URL = '/wasm/opus-wasm/opus-decoder.wasm';
    const SAMPLE_RATE = 48000;
    const MAX_FRAME_SIZE = 5760; // 120ms at 48kHz (max Opus frame)
    const MAX_CHANNELS = 2;

    // ── State ──────────────────────────────────────────────
    let _wasmModule = null;
    let _wasmMemory = null;
    let _decoder = null;       // Pointer to OpusDecoder in WASM heap
    let _initialized = false;
    let _available = false;    // Set true after successful WASM load
    let _channels = 2;

    // WASM function pointers (set after instantiation)
    let _opus_decoder_get_size = null;
    let _opus_decoder_init = null;
    let _opus_decode_float = null;
    let _malloc = null;
    let _free = null;

    // Persistent WASM heap buffers
    let _inputPtr = null;       // For encoded Opus data
    let _outputPtr = null;      // For decoded f32 PCM
    let _inputBufSize = 0;
    let _outputBufSize = 0;

    // ── WASM Memory Helpers ────────────────────────────────

    function ensureInputBuffer(size) {
        if (_inputPtr && _inputBufSize >= size) return;
        if (_inputPtr) _free(_inputPtr);
        _inputBufSize = Math.max(size, 4096); // Minimum 4KB
        _inputPtr = _malloc(_inputBufSize);
        if (!_inputPtr) throw new Error('Failed to allocate WASM input buffer');
    }

    function ensureOutputBuffer(frames, channels) {
        var needed = frames * channels * 4; // f32 = 4 bytes
        if (_outputPtr && _outputBufSize >= needed) return;
        if (_outputPtr) _free(_outputPtr);
        _outputBufSize = Math.max(needed, MAX_FRAME_SIZE * MAX_CHANNELS * 4);
        _outputPtr = _malloc(_outputBufSize);
        if (!_outputPtr) throw new Error('Failed to allocate WASM output buffer');
    }

    // ── Build minimal WASM module from C source via Emscripten ──
    // The WASM module exports these libopus functions:
    //   opus_decoder_get_size(channels) -> int
    //   opus_decoder_init(decoder_ptr, sample_rate, channels) -> int
    //   opus_decode_float(decoder_ptr, data_ptr, data_len, pcm_ptr, frame_size, decode_fec) -> int
    //   malloc(size) -> ptr
    //   free(ptr)
    //
    // We wrap them in a thin layer here.

    /**
     * Load and instantiate the libopus WASM module.
     * Returns true on success, false if the WASM file is not available.
     */
    async function init(channels) {
        if (_initialized) return _available;

        _channels = channels || 2;

        try {
            // Try to load the pre-compiled WASM
            var resp = await fetch(OPUS_WASM_URL);
            if (!resp.ok) {
                console.warn('[SolunaOpusWasm] opus-decoder.wasm not found (HTTP ' + resp.status + '). Fallback disabled.');
                _initialized = true;
                _available = false;
                return false;
            }

            var wasmBytes = await resp.arrayBuffer();

            // Emscripten-style minimal imports
            var memory = new WebAssembly.Memory({ initial: 256, maximum: 2048 }); // 16MB - 128MB
            var importObject = {
                env: {
                    memory: memory,
                    // Emscripten runtime stubs (libopus doesn't use most of them)
                    emscripten_notify_memory_growth: function() {},
                    __assert_fail: function(condition, filename, line, func) {
                        console.error('[SolunaOpusWasm] assertion failed');
                    },
                    abort: function() {
                        throw new Error('[SolunaOpusWasm] WASM abort');
                    },
                },
                wasi_snapshot_preview1: {
                    // libopus doesn't do I/O, but some builds require these stubs
                    proc_exit: function(code) {},
                    fd_close: function() { return 0; },
                    fd_write: function() { return 0; },
                    fd_seek: function() { return 0; },
                },
            };

            var result;
            try {
                // Try instantiateStreaming first (not applicable for arrayBuffer,
                // but some environments support it)
                result = await WebAssembly.instantiate(wasmBytes, importObject);
            } catch (e) {
                console.warn('[SolunaOpusWasm] WASM instantiation failed:', e.message);
                _initialized = true;
                _available = false;
                return false;
            }

            _wasmModule = result.instance;

            // If the module exports its own memory, use that instead
            if (_wasmModule.exports.memory) {
                _wasmMemory = _wasmModule.exports.memory;
            } else {
                _wasmMemory = memory;
            }

            // Bind exported functions
            _opus_decoder_get_size = _wasmModule.exports.opus_decoder_get_size;
            _opus_decoder_init = _wasmModule.exports.opus_decoder_init;
            _opus_decode_float = _wasmModule.exports.opus_decode_float;
            _malloc = _wasmModule.exports.malloc;
            _free = _wasmModule.exports.free;

            // Validate all required exports exist
            if (!_opus_decoder_get_size || !_opus_decoder_init ||
                !_opus_decode_float || !_malloc || !_free) {
                console.warn('[SolunaOpusWasm] Missing required WASM exports. Need: opus_decoder_get_size, opus_decoder_init, opus_decode_float, malloc, free');
                _initialized = true;
                _available = false;
                return false;
            }

            // Initialize the Opus decoder
            var decoderSize = _opus_decoder_get_size(_channels);
            _decoder = _malloc(decoderSize);
            if (!_decoder) {
                console.error('[SolunaOpusWasm] Failed to allocate decoder');
                _initialized = true;
                _available = false;
                return false;
            }

            var err = _opus_decoder_init(_decoder, SAMPLE_RATE, _channels);
            if (err !== 0) {
                console.error('[SolunaOpusWasm] opus_decoder_init failed with error:', err);
                _free(_decoder);
                _decoder = null;
                _initialized = true;
                _available = false;
                return false;
            }

            // Pre-allocate buffers
            ensureInputBuffer(4096);
            ensureOutputBuffer(MAX_FRAME_SIZE, _channels);

            _initialized = true;
            _available = true;
            console.log('[SolunaOpusWasm] libopus WASM decoder initialized (' + _channels + 'ch, ' + SAMPLE_RATE + 'Hz)');
            return true;

        } catch (e) {
            console.warn('[SolunaOpusWasm] Init failed:', e.message);
            _initialized = true;
            _available = false;
            return false;
        }
    }

    /**
     * Decode an Opus packet to f32 PCM.
     *
     * @param {Uint8Array} opusData - Raw Opus packet bytes
     * @param {number} frameSize - Expected frame size in samples per channel
     *                             (e.g., 960 for 20ms at 48kHz). Pass 0 to auto-detect.
     * @returns {Float32Array|null} - Interleaved f32 PCM, or null on error
     */
    function decode(opusData, frameSize) {
        if (!_available || !_decoder) return null;

        // Default frame size: 20ms at 48kHz = 960 samples
        if (!frameSize || frameSize <= 0) {
            frameSize = 960;
        }

        // Clamp frame size to max
        if (frameSize > MAX_FRAME_SIZE) {
            frameSize = MAX_FRAME_SIZE;
        }

        try {
            // Copy input data to WASM heap
            ensureInputBuffer(opusData.length);
            var heap8 = new Uint8Array(_wasmMemory.buffer);
            heap8.set(opusData, _inputPtr);

            // Ensure output buffer is large enough
            ensureOutputBuffer(frameSize, _channels);

            // Call opus_decode_float
            // int opus_decode_float(OpusDecoder *st, const unsigned char *data,
            //                       opus_int32 len, float *pcm,
            //                       int frame_size, int decode_fec)
            var samplesDecoded = _opus_decode_float(
                _decoder,
                _inputPtr,
                opusData.length,
                _outputPtr,
                frameSize,
                0  // decode_fec = 0 (no forward error correction)
            );

            if (samplesDecoded < 0) {
                // Opus error codes:
                // -1 = OPUS_BAD_ARG, -2 = OPUS_BUFFER_TOO_SMALL,
                // -3 = OPUS_INTERNAL_ERROR, -4 = OPUS_INVALID_PACKET,
                // -5 = OPUS_UNIMPLEMENTED, -6 = OPUS_INVALID_STATE
                if (samplesDecoded !== -4) { // Suppress common invalid packet errors
                    console.warn('[SolunaOpusWasm] decode error:', samplesDecoded);
                }
                return null;
            }

            // Copy decoded f32 PCM from WASM heap
            var totalSamples = samplesDecoded * _channels;
            var heapF32 = new Float32Array(_wasmMemory.buffer);
            var pcmOffset = _outputPtr / 4; // f32 offset
            var result = new Float32Array(totalSamples);
            result.set(heapF32.subarray(pcmOffset, pcmOffset + totalSamples));

            return result;

        } catch (e) {
            console.warn('[SolunaOpusWasm] decode exception:', e.message);
            return null;
        }
    }

    /**
     * Decode a packet for packet loss concealment (PLC).
     * Called when a packet is lost to generate interpolated audio.
     *
     * @param {number} frameSize - Frame size in samples per channel
     * @returns {Float32Array|null} - Interpolated f32 PCM, or null on error
     */
    function decodePlc(frameSize) {
        if (!_available || !_decoder) return null;

        frameSize = frameSize || 960;
        if (frameSize > MAX_FRAME_SIZE) frameSize = MAX_FRAME_SIZE;

        try {
            ensureOutputBuffer(frameSize, _channels);

            // Pass null data pointer + 0 length = PLC mode
            var samplesDecoded = _opus_decode_float(
                _decoder,
                0,     // null data = PLC
                0,     // 0 length
                _outputPtr,
                frameSize,
                0
            );

            if (samplesDecoded < 0) return null;

            var totalSamples = samplesDecoded * _channels;
            var heapF32 = new Float32Array(_wasmMemory.buffer);
            var pcmOffset = _outputPtr / 4;
            var result = new Float32Array(totalSamples);
            result.set(heapF32.subarray(pcmOffset, pcmOffset + totalSamples));

            return result;
        } catch (e) {
            return null;
        }
    }

    /**
     * Reset the decoder state (e.g., after a seek or stream switch).
     */
    function reset() {
        if (!_available || !_decoder) return;
        // Re-init the decoder (cheapest way to reset state)
        _opus_decoder_init(_decoder, SAMPLE_RATE, _channels);
    }

    /**
     * Clean up all WASM resources.
     */
    function destroy() {
        if (_inputPtr && _free) { _free(_inputPtr); _inputPtr = null; }
        if (_outputPtr && _free) { _free(_outputPtr); _outputPtr = null; }
        if (_decoder && _free) { _free(_decoder); _decoder = null; }
        _inputBufSize = 0;
        _outputBufSize = 0;
        _available = false;
        _initialized = false;
        _wasmModule = null;
        _wasmMemory = null;
    }

    /**
     * Whether the WASM Opus decoder is available and initialized.
     */
    function isAvailable() {
        return _available;
    }

    /**
     * Get the channel count configured for this decoder.
     */
    function getChannels() {
        return _channels;
    }

    // ── Public API ─────────────────────────────────────────

    globalThis.SolunaOpusWasm = {
        init: init,
        decode: decode,
        decodePlc: decodePlc,
        reset: reset,
        destroy: destroy,
        isAvailable: isAvailable,
        getChannels: getChannels,
    };

    console.log('[SolunaOpusWasm] Loaded. Call SolunaOpusWasm.init() to initialize.');
})();
