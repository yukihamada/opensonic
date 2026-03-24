/**
 * Web Audio API player for decoded PCM audio from the Soluna relay.
 * Format: 48kHz stereo float32.
 */
import { decodeADPCMPayload } from "./ADPCMCodec.js";
import { OSTConstants } from "./OSTPacketParser.js";
const SAMPLE_RATE = 48000;
export class AudioPlayer {
    ctx = null;
    nextTime = 0;
    _isPlaying = false;
    get isPlaying() {
        return this._isPlaying;
    }
    /** Start the AudioContext. Must be called from a user gesture on most browsers. */
    start() {
        if (this.ctx)
            return;
        this.ctx = new AudioContext({ sampleRate: SAMPLE_RATE });
        this.nextTime = 0;
        this._isPlaying = true;
    }
    /** Stop and close the AudioContext. */
    stop() {
        if (this.ctx) {
            this.ctx.close();
            this.ctx = null;
        }
        this._isPlaying = false;
        this.nextTime = 0;
    }
    /** Schedule a parsed packet for playback. */
    playPacket(packet) {
        if (!this.ctx)
            return;
        let floatSamples;
        if (packet.payloadType === OSTConstants.ptADPCMStereo ||
            packet.payloadType === OSTConstants.ptADPCMMono) {
            floatSamples = this.decodeADPCMToFloat(packet.payload, packet.channels);
        }
        else {
            floatSamples = this.decodeInt32LEToFloat(packet.payload, packet.channels);
        }
        if (floatSamples.length === 0)
            return;
        const channels = Math.min(packet.channels, 2);
        const framesPerChannel = Math.floor(floatSamples.length / channels);
        if (framesPerChannel === 0)
            return;
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
    /** Decode int32 LE interleaved payload to float32. */
    decodeInt32LEToFloat(payload, channels) {
        const totalSamples = Math.floor(payload.length / 4);
        if (totalSamples === 0)
            return new Float32Array(0);
        const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
        const out = new Float32Array(totalSamples);
        const scale = 1.0 / 2147483647; // 1 / Int32.max
        for (let i = 0; i < totalSamples; i++) {
            out[i] = view.getInt32(i * 4, true) * scale; // true = little-endian
        }
        return out;
    }
    /** Decode ADPCM payload to float32. */
    decodeADPCMToFloat(payload, channels) {
        const pcm16 = decodeADPCMPayload(payload);
        if (!pcm16)
            return new Float32Array(0);
        const out = new Float32Array(pcm16.length);
        for (let i = 0; i < pcm16.length; i++) {
            out[i] = pcm16[i] / 32768.0;
        }
        return out;
    }
}
