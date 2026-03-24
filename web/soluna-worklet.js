// AudioWorklet processor for Soluna radio playback (stereo)
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
    this.prefillThreshold = 9600; // 200ms

    this.port.onmessage = (e) => {
      if (e.data.type === 'stereo') {
        const L = e.data.l;
        const R = e.data.r;
        const len = L.length;
        if (this.buffered > this.cap * 0.9) return; // drop if nearly full
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
        // Mono fallback (backward compat)
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
      }
    };
  }

  process(inputs, outputs) {
    const outL = outputs[0][0];
    const outR = outputs[0][1] || outputs[0][0]; // fallback to mono if 1ch output
    if (!outL) return true;

    if (!this.prefillReached) {
      outL.fill(0);
      if (outR !== outL) outR.fill(0);
      return true;
    }

    for (let i = 0; i < outL.length; i++) {
      if (this.buffered > 0) {
        outL[i] = this.ringL[this.readPos];
        outR[i] = this.ringR[this.readPos];
        this.readPos = (this.readPos + 1) % this.cap;
        this.buffered--;
      } else {
        outL[i] = 0;
        outR[i] = 0;
      }
    }
    return true;
  }
}

registerProcessor('soluna-processor', SolunaProcessor);
