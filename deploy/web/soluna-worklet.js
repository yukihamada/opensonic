// AudioWorklet processor for Soluna radio playback (stereo)
// Adaptive rate control: maintains target buffer level for cross-device sync
class SolunaProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.cap = 192000; // 4s @ 48kHz per channel
    this.ringL = new Float32Array(this.cap);
    this.ringR = new Float32Array(this.cap);
    this.writePos = 0;
    this.readPos = 0;
    this.buffered = 0;
    this.prefillReached = false;
    this.prefillThreshold = 14400; // 300ms initial prefill

    // Adaptive sync: target buffer level (set by main thread based on media_timestamp)
    this.targetBufferFrames = 14400; // 300ms default, updated dynamically
    // Fractional read position for smooth rate adjustment
    this.fracPos = 0.0;

    this.port.onmessage = (e) => {
      if (e.data.type === 'stereo') {
        const L = e.data.l;
        const R = e.data.r;
        const len = L.length;
        if (this.buffered > this.cap * 0.9) return;
        for (let i = 0; i < len; i++) {
          this.ringL[this.writePos] = L[i];
          this.ringR[this.writePos] = R[i];
          this.writePos = (this.writePos + 1) % this.cap;
        }
        this.buffered += len;
        if (!this.prefillReached && this.buffered >= this.prefillThreshold) {
          this.prefillReached = true;
        }
      } else if (e.data.type === 'samples') {
        const samples = e.data.samples;
        if (this.buffered > this.cap * 0.9) return;
        for (let i = 0; i < samples.length; i++) {
          this.ringL[this.writePos] = samples[i];
          this.ringR[this.writePos] = samples[i];
          this.writePos = (this.writePos + 1) % this.cap;
        }
        this.buffered += samples.length;
        if (!this.prefillReached && this.buffered >= this.prefillThreshold) {
          this.prefillReached = true;
        }
      } else if (e.data.type === 'reset') {
        this.writePos = 0;
        this.readPos = 0;
        this.buffered = 0;
        this.prefillReached = false;
        this.fracPos = 0.0;
      } else if (e.data.type === 'setTarget') {
        // Main thread sets target buffer based on media_timestamp sync
        this.targetBufferFrames = e.data.frames || 14400;
      } else if (e.data.type === 'getBuffered') {
        this.port.postMessage({ type: 'buffered', frames: this.buffered });
      } else if (e.data.type === 'skip') {
        var skip = Math.min(e.data.frames || 0, this.buffered);
        this.readPos = (this.readPos + skip) % this.cap;
        this.buffered -= skip;
      }
    };
  }

  process(inputs, outputs) {
    const outL = outputs[0][0];
    const outR = outputs[0][1] || outputs[0][0];
    if (!outL) return true;

    if (!this.prefillReached) {
      outL.fill(0);
      if (outR !== outL) outR.fill(0);
      return true;
    }

    // Adaptive rate: adjust consumption speed to maintain target buffer level
    // rate > 1.0 = consume faster (buffer too high, we're behind)
    // rate < 1.0 = consume slower (buffer too low, we're ahead)
    const diff = this.buffered - this.targetBufferFrames;
    // Gentle correction: ±0.1% per 100 frames difference, clamped to ±2%
    const correction = Math.max(-0.02, Math.min(0.02, diff * 0.000001));
    const rate = 1.0 + correction;

    const len = outL.length;
    for (let i = 0; i < len; i++) {
      if (this.buffered > 0) {
        // Integer part of read position
        const idx = this.readPos;
        outL[i] = this.ringL[idx];
        outR[i] = this.ringR[idx];

        // Advance by rate (normally 1.0, slightly more/less for sync)
        this.fracPos += rate;
        const advance = this.fracPos | 0; // integer part
        this.fracPos -= advance;
        // Skip or repeat samples based on rate
        const actualAdvance = Math.min(advance, this.buffered);
        this.readPos = (this.readPos + actualAdvance) % this.cap;
        this.buffered -= actualAdvance;
      } else {
        outL[i] = 0;
        outR[i] = 0;
      }
    }

    // Report buffer level every ~1s (48000/128 ≈ 375 calls/s, report every 375th)
    if (Math.random() < 0.003) {
      this.port.postMessage({ type: 'buffered', frames: this.buffered });
    }

    return true;
  }
}

registerProcessor('soluna-processor', SolunaProcessor);
