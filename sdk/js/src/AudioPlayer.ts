/**
 * Web Audio API player for decoded PCM audio from the Soluna relay.
 * Format: 48kHz stereo float32.
 */

import { decodeADPCMPayload } from "./ADPCMCodec.js";
import { OSTConstants } from "./OSTPacketParser.js";
import type { OSTPacket } from "./OSTPacketParser.js";

const SAMPLE_RATE = 48000;

export class AudioPlayer {
  private ctx: AudioContext | null = null;
  private nextTime = 0;
  private _isPlaying = false;

  get isPlaying(): boolean {
    return this._isPlaying;
  }

  /** Start the AudioContext. Must be called from a user gesture on most browsers. */
  start(): void {
    if (this.ctx) return;
    this.ctx = new AudioContext({ sampleRate: SAMPLE_RATE });
    this.nextTime = 0;
    this._isPlaying = true;
  }

  /** Stop and close the AudioContext. */
  stop(): void {
    if (this.ctx) {
      this.ctx.close();
      this.ctx = null;
    }
    this._isPlaying = false;
    this.nextTime = 0;
  }

  /** Schedule a parsed packet for playback. */
  playPacket(packet: OSTPacket): void {
    if (!this.ctx) return;

    let floatSamples: Float32Array;

    if (
      packet.payloadType === OSTConstants.ptADPCMStereo ||
      packet.payloadType === OSTConstants.ptADPCMMono
    ) {
      floatSamples = this.decodeADPCMToFloat(packet.payload, packet.channels);
    } else if (packet.payloadType === OSTConstants.ptOpus) {
      const decoded = this.decodeOpusToFloat(packet.payload, packet.channels);
      if (decoded) {
        this.scheduleDecodedBuffer(decoded.buffer, decoded.frames, packet.channels);
      }
      return;
    } else if (packet.payloadType === OSTConstants.ptLC3) {
      console.warn("[SolunaSDK] LC3 codec not supported in browser — install liblc3 WASM module");
      return;
    } else {
      floatSamples = this.decodeInt32LEToFloat(packet.payload, packet.channels);
    }

    if (floatSamples.length === 0) return;

    const channels = Math.min(packet.channels, 2);
    const framesPerChannel = Math.floor(floatSamples.length / channels);
    if (framesPerChannel === 0) return;

    const buffer = this.ctx.createBuffer(2, framesPerChannel, SAMPLE_RATE);

    for (let ch = 0; ch < Math.min(channels, 2); ch++) {
      const channelData = buffer.getChannelData(ch);
      for (let f = 0; f < framesPerChannel; f++) {
        channelData[f] = floatSamples[f * channels + ch];
      }
    }

    // Mono -> stereo: copy left to right
    if (channels === 1) {
      const left = buffer.getChannelData(0);
      const right = buffer.getChannelData(1);
      right.set(left);
    }

    const source = this.ctx.createBufferSource();
    source.buffer = buffer;
    source.connect(this.ctx.destination);

    const currentTime = this.ctx.currentTime;
    if (this.nextTime < currentTime) {
      this.nextTime = currentTime;
    }
    source.start(this.nextTime);
    this.nextTime += framesPerChannel / SAMPLE_RATE;
  }

  /**
   * Schedule a pre-decoded AudioBuffer for playback.
   * Used by async decoders (Opus via decodeAudioData).
   */
  private scheduleDecodedBuffer(buffer: AudioBuffer, frames: number, channels: number): void {
    if (!this.ctx) return;

    const source = this.ctx.createBufferSource();
    source.buffer = buffer;
    source.connect(this.ctx.destination);

    const currentTime = this.ctx.currentTime;
    if (this.nextTime < currentTime) {
      this.nextTime = currentTime;
    }
    source.start(this.nextTime);
    this.nextTime += frames / SAMPLE_RATE;
  }

  /**
   * Decode Opus payload to float32 via decodeAudioData with minimal Ogg wrapper.
   * Browsers can decode Opus natively when wrapped in an Ogg container.
   * Returns null if the AudioContext is not available.
   *
   * Note: decodeAudioData is async; this schedules playback on completion.
   */
  private decodeOpusToFloat(
    payload: Uint8Array,
    channels: number
  ): { buffer: AudioBuffer; frames: number } | null {
    if (!this.ctx) return null;

    const ctx = this.ctx;
    const nextTimeRef = { value: this.nextTime };

    // Wrap raw Opus frame in a minimal Ogg container for decodeAudioData.
    // Build a single-page Ogg/Opus container.
    const oggData = this.wrapOpusInOgg(payload, channels);

    ctx.decodeAudioData(oggData.buffer.slice(0)).then((audioBuffer) => {
      const source = ctx.createBufferSource();
      source.buffer = audioBuffer;
      source.connect(ctx.destination);

      const currentTime = ctx.currentTime;
      if (this.nextTime < currentTime) {
        this.nextTime = currentTime;
      }
      source.start(this.nextTime);
      this.nextTime += audioBuffer.length / SAMPLE_RATE;
    }).catch(() => {
      // Opus decode failed — browser may not support this approach
      console.warn("[SolunaSDK] Opus decodeAudioData failed; consider using opus-decoder npm package");
    });

    // Return null since decode is async; playback is scheduled in the promise
    return null;
  }

  /**
   * Wrap a raw Opus frame in a minimal Ogg container (OpusHead + OpusTags + audio page).
   * This allows browsers to decode via decodeAudioData.
   */
  private wrapOpusInOgg(opusFrame: Uint8Array, channels: number): Uint8Array {
    // OpusHead: 19 bytes
    const opusHead = new Uint8Array(19);
    opusHead.set([0x4F, 0x70, 0x75, 0x73, 0x48, 0x65, 0x61, 0x64]); // "OpusHead"
    opusHead[8] = 1;    // version
    opusHead[9] = channels;
    // pre-skip: 3840 (little-endian)
    opusHead[10] = 0x00; opusHead[11] = 0x0F;
    // sample rate: 48000 (little-endian)
    opusHead[12] = 0x80; opusHead[13] = 0xBB; opusHead[14] = 0x00; opusHead[15] = 0x00;
    // output gain
    opusHead[16] = 0x00; opusHead[17] = 0x00;
    // channel mapping family
    opusHead[18] = 0;

    // OpusTags: minimal
    const opusTags = new Uint8Array(16);
    opusTags.set([0x4F, 0x70, 0x75, 0x73, 0x54, 0x61, 0x67, 0x73]); // "OpusTags"
    // vendor string length: 0
    opusTags[8] = 0; opusTags[9] = 0; opusTags[10] = 0; opusTags[11] = 0;
    // comment count: 0
    opusTags[12] = 0; opusTags[13] = 0; opusTags[14] = 0; opusTags[15] = 0;

    // Build 3 Ogg pages: head, tags, audio
    const page0 = this.buildOggPage(opusHead, 0, 0, 2, true);  // BOS
    const page1 = this.buildOggPage(opusTags, 0, 1, 0, false);
    const page2 = this.buildOggPage(opusFrame, 0, 2, 4, false); // EOS

    const result = new Uint8Array(page0.length + page1.length + page2.length);
    result.set(page0, 0);
    result.set(page1, page0.length);
    result.set(page2, page0.length + page1.length);
    return result;
  }

  /**
   * Build a single Ogg page with the given payload.
   */
  private buildOggPage(
    payload: Uint8Array,
    granulePos: number,
    pageSeqNo: number,
    headerType: number,
    isBOS: boolean
  ): Uint8Array {
    const segCount = Math.ceil(payload.length / 255) || 1;
    const headerSize = 27 + segCount;
    const page = new Uint8Array(headerSize + payload.length);

    // Capture pattern: "OggS"
    page.set([0x4F, 0x67, 0x67, 0x53], 0);
    // Version
    page[4] = 0;
    // Header type
    page[5] = headerType;
    // Granule position (8 bytes, little-endian)
    const view = new DataView(page.buffer);
    view.setUint32(6, granulePos, true);
    view.setUint32(10, 0, true);
    // Serial number
    view.setUint32(14, 1, true);
    // Page sequence number
    view.setUint32(18, pageSeqNo, true);
    // CRC placeholder (will be filled below)
    view.setUint32(22, 0, true);
    // Segment count
    page[26] = segCount;
    // Segment table
    let remaining = payload.length;
    for (let i = 0; i < segCount; i++) {
      page[27 + i] = remaining > 255 ? 255 : remaining;
      remaining -= Math.min(remaining, 255);
    }
    // Payload
    page.set(payload, headerSize);

    // CRC-32 (Ogg variant)
    const crc = this.oggCrc32(page);
    view.setUint32(22, crc, true);

    return page;
  }

  /** Ogg CRC-32 (polynomial 0x04C11DB7, no bit reversal). */
  private oggCrc32(data: Uint8Array): number {
    let crc = 0;
    for (let i = 0; i < data.length; i++) {
      crc = ((crc << 8) ^ this.oggCrcTable[((crc >>> 24) ^ data[i]) & 0xFF]) >>> 0;
    }
    return crc;
  }

  private oggCrcTable: number[] = (() => {
    const table: number[] = [];
    for (let i = 0; i < 256; i++) {
      let r = i << 24;
      for (let j = 0; j < 8; j++) {
        r = (r & 0x80000000) ? ((r << 1) ^ 0x04C11DB7) : (r << 1);
        r = r >>> 0;
      }
      table.push(r >>> 0);
    }
    return table;
  })();

  /** Decode int32 LE interleaved payload to float32. */
  private decodeInt32LEToFloat(payload: Uint8Array, channels: number): Float32Array {
    const totalSamples = Math.floor(payload.length / 4);
    if (totalSamples === 0) return new Float32Array(0);

    const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    const out = new Float32Array(totalSamples);
    const scale = 1.0 / 2147483647; // 1 / Int32.max

    for (let i = 0; i < totalSamples; i++) {
      out[i] = view.getInt32(i * 4, true) * scale; // true = little-endian
    }
    return out;
  }

  /** Decode ADPCM payload to float32. */
  private decodeADPCMToFloat(payload: Uint8Array, channels: number): Float32Array {
    const pcm16 = decodeADPCMPayload(payload);
    if (!pcm16) return new Float32Array(0);

    const out = new Float32Array(pcm16.length);
    for (let i = 0; i < pcm16.length; i++) {
      out[i] = pcm16[i] / 32768.0;
    }
    return out;
  }
}
