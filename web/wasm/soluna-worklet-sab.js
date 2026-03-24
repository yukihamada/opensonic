/**
 * Soluna AudioWorklet Processor — SharedArrayBuffer zero-copy variant
 *
 * Reads stereo PCM directly from a SharedArrayBuffer written by the main thread.
 * No postMessage for audio data; only infrequent stats messages.
 *
 * SAB Layout (total: 8 + CAPACITY*2*4 bytes):
 *   Offset 0:  writeHead (Uint32, atomically updated by main thread)
 *   Offset 4:  readHead  (Uint32, atomically updated by worklet)
 *   Offset 8:  L channel Float32 data [CAPACITY samples]
 *   Offset 8 + CAPACITY*4:  R channel Float32 data [CAPACITY samples]
 *
 * SPSC (single-producer single-consumer) lock-free ring buffer:
 *   - Main thread is the sole writer (advances writeHead)
 *   - Worklet is the sole reader (advances readHead)
 *   - Atomics.load/store provide acquire/release ordering
 *
 * Requires: Cross-Origin-Opener-Policy: same-origin
 *           Cross-Origin-Embedder-Policy: require-corp
 */
class SolunaWasmSabProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this._ready = false;
        this._cap = 0;
        this._headView = null;  // Int32Array over first 8 bytes
        this._L = null;         // Float32Array over L channel region
        this._R = null;         // Float32Array over R channel region
        this._tick = 0;

        this.port.onmessage = ({ data }) => {
            if (data.type === 'init-sab') {
                const sab = data.sab;
                const cap = data.capacity; // number of samples per channel
                this._cap = cap;
                this._headView = new Int32Array(sab, 0, 2); // [writeHead, readHead]
                this._L = new Float32Array(sab, 8, cap);
                this._R = new Float32Array(sab, 8 + cap * 4, cap);
                this._ready = true;
            }
        };
    }

    process(inputs, outputs) {
        const L = outputs[0][0];
        const R = outputs[0][1] || outputs[0][0];
        const n = L.length; // 128

        if (!this._ready) {
            L.fill(0);
            R.fill(0);
            return true;
        }

        const w = Atomics.load(this._headView, 0);
        const r = Atomics.load(this._headView, 1);
        const cap = this._cap;

        // Available samples: handle wrap-around via unsigned difference
        const avail = (w - r + cap) % cap;

        if (avail < n) {
            // Underrun
            L.fill(0);
            R.fill(0);
            this.port.postMessage({ type: 'underrun' });
        } else {
            const rMasked = r % cap;
            // Check if we can do a contiguous copy (no wrap)
            if (rMasked + n <= cap) {
                L.set(this._L.subarray(rMasked, rMasked + n));
                R.set(this._R.subarray(rMasked, rMasked + n));
            } else {
                // Wrap-around: copy in two parts
                const first = cap - rMasked;
                L.set(this._L.subarray(rMasked, cap), 0);
                L.set(this._L.subarray(0, n - first), first);
                R.set(this._R.subarray(rMasked, cap), 0);
                R.set(this._R.subarray(0, n - first), first);
            }
            Atomics.store(this._headView, 1, (r + n) % cap);
        }

        // Stats every ~1s (375 * 128 samples @ 48kHz)
        if (++this._tick >= 375) {
            this._tick = 0;
            const wNow = Atomics.load(this._headView, 0);
            const rNow = Atomics.load(this._headView, 1);
            const fill = (wNow - rNow + cap) % cap;
            this.port.postMessage({ type: 'stats', fill: fill });
        }

        return true;
    }
}

registerProcessor('soluna-wasm-sab', SolunaWasmSabProcessor);
