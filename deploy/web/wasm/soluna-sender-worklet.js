/**
 * soluna-sender-worklet.js — AudioWorklet processor for SolunaSender
 * Accumulates input samples into 960-sample frames (20 ms at 48 kHz)
 * and posts each frame to the main thread via MessagePort.
 *
 * SPDX-License-Identifier: MIT
 */

class SolunaSenderProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this._buf = [];
  }

  /**
   * @param {Float32Array[][]} inputs  - inputs[0][0] = mono channel data
   * @returns {boolean} Keep processor alive.
   */
  process(inputs) {
    const input = inputs[0]?.[0];
    if (!input) return true;

    // Accumulate samples
    for (let i = 0; i < input.length; i++) {
      this._buf.push(input[i]);
    }

    // Emit complete 960-sample frames (20 ms @ 48 kHz)
    while (this._buf.length >= 960) {
      this.port.postMessage(new Float32Array(this._buf.splice(0, 960)));
    }

    return true;
  }
}

registerProcessor('soluna-sender-processor', SolunaSenderProcessor);
