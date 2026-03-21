// AudioWorklet processor for Soluna radio playback
// Reads from a shared ring buffer (posted via port messages)

class SolunaProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ring = new Float32Array(192000); // 4s @ 48kHz
    this.writePos = 0;
    this.readPos = 0;
    this.buffered = 0;
    this.prefillReached = false;
    this.prefillThreshold = 9600; // 200ms

    this.port.onmessage = (e) => {
      if (e.data.type === 'samples') {
        const samples = e.data.samples;
        const cap = this.ring.length;
        // Drop if nearly full
        if (this.buffered > cap * 0.9) return;
        for (let i = 0; i < samples.length; i++) {
          this.ring[this.writePos] = samples[i];
          this.writePos = (this.writePos + 1) % cap;
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
    const output = outputs[0][0];
    if (!output) return true;

    if (!this.prefillReached) {
      output.fill(0);
      return true;
    }

    for (let i = 0; i < output.length; i++) {
      if (this.buffered > 0) {
        output[i] = this.ring[this.readPos];
        this.readPos = (this.readPos + 1) % this.ring.length;
        this.buffered--;
      } else {
        output[i] = 0;
      }
    }
    return true;
  }
}

registerProcessor('soluna-processor', SolunaProcessor);
