/**
 * Soluna AudioWorklet Processor (WASM-backed)
 *
 * Receives audio data via postMessage from the main thread,
 * and pulls processed audio from a shared buffer for playback.
 *
 * Protocol:
 *   Main thread → Worklet:  { type: 'audio', l: Float32Array, r: Float32Array }
 *   Worklet → Main thread:  { type: 'underrun' } | { type: 'stats', fill: number }
 */
class SolunaWasmProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this._cap = 131072; // ~2.7s at 48kHz
        this._L = new Float32Array(this._cap);
        this._R = new Float32Array(this._cap);
        this._w = 0;
        this._r = 0;
        this._tick = 0;

        this.port.onmessage = ({ data }) => {
            if (data.type === 'audio') {
                const l = data.l;
                const r = data.r;
                for (let i = 0; i < l.length; i++) {
                    const idx = this._w % this._cap;
                    this._L[idx] = l[i];
                    this._R[idx] = r[i];
                    this._w++;
                }
            }
        };
    }

    process(inputs, outputs) {
        const L = outputs[0][0];
        const R = outputs[0][1] || outputs[0][0];
        const n = L.length; // 128
        const avail = this._w - this._r;

        if (avail < n) {
            L.fill(0);
            R.fill(0);
            this.port.postMessage({ type: 'underrun' });
        } else {
            for (let i = 0; i < n; i++) {
                const p = (this._r + i) % this._cap;
                L[i] = this._L[p];
                R[i] = this._R[p];
            }
            this._r += n;
        }

        // Stats every ~1s
        if (++this._tick >= 375) {
            this._tick = 0;
            this.port.postMessage({ type: 'stats', fill: this._w - this._r });
        }

        return true;
    }
}

registerProcessor('soluna-wasm', SolunaWasmProcessor);
