/**
 * Web Audio API player for decoded PCM audio from the Soluna relay.
 * Format: 48kHz stereo float32.
 */
import type { OSTPacket } from "./OSTPacketParser.js";
export declare class AudioPlayer {
    private ctx;
    private nextTime;
    private _isPlaying;
    get isPlaying(): boolean;
    /** Start the AudioContext. Must be called from a user gesture on most browsers. */
    start(): void;
    /** Stop and close the AudioContext. */
    stop(): void;
    /** Schedule a parsed packet for playback. */
    playPacket(packet: OSTPacket): void;
    /** Decode int32 LE interleaved payload to float32. */
    private decodeInt32LEToFloat;
    /** Decode ADPCM payload to float32. */
    private decodeADPCMToFloat;
}
