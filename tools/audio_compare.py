#!/usr/bin/env python3
"""
audio_compare.py — TX vs RX audio quality comparison tool

Compares the transmitted audio (TX WAV) with received audio (RX WAV) to
identify quality issues: packet loss, ASRC drift, noise, distortion.

Usage:
    python3 audio_compare.py tx.wav rx.wav [--plot]

Requirements:
    pip3 install numpy scipy
    pip3 install matplotlib  # optional, for --plot

Output:
    - Timing offset (ms) between TX and RX
    - SNR (dB) — signal-to-noise ratio of received vs sent
    - Correlation — peak cross-correlation (1.0 = perfect)
    - Dropout count — detected silence gaps in RX
    - Sample drift — ASRC-induced sample count mismatch
    - Spectral difference — frequency domain comparison
    - Recommendations for tuning
"""

import sys
import struct
import argparse
import numpy as np


def read_wav(path: str) -> tuple:
    """Read a WAV file, return (samples_int16[], sample_rate, channels)."""
    with open(path, 'rb') as f:
        riff = f.read(4)
        if riff != b'RIFF':
            raise ValueError(f"Not a WAV file: {path}")
        f.read(4)  # file size
        wave = f.read(4)
        if wave != b'WAVE':
            raise ValueError(f"Not a WAV file: {path}")

        fmt_found = False
        sample_rate = 0
        channels = 0
        bits = 0
        data = b''

        while True:
            chunk_id = f.read(4)
            if len(chunk_id) < 4:
                break
            chunk_size = struct.unpack('<I', f.read(4))[0]
            if chunk_id == b'fmt ':
                fmt_data = f.read(chunk_size)
                audio_fmt = struct.unpack('<H', fmt_data[0:2])[0]
                channels = struct.unpack('<H', fmt_data[2:4])[0]
                sample_rate = struct.unpack('<I', fmt_data[4:8])[0]
                bits = struct.unpack('<H', fmt_data[14:16])[0]
                fmt_found = True
            elif chunk_id == b'data':
                data = f.read(chunk_size)
            else:
                f.read(chunk_size)

        if not fmt_found:
            raise ValueError(f"No fmt chunk in {path}")

        if bits == 16:
            samples = np.frombuffer(data, dtype=np.int16)
        elif bits == 24:
            # 24-bit: unpack 3 bytes per sample
            n_samples = len(data) // 3
            samples = np.zeros(n_samples, dtype=np.int32)
            for i in range(n_samples):
                b = data[i*3:(i+1)*3]
                val = b[0] | (b[1] << 8) | (b[2] << 16)
                if val & 0x800000:
                    val -= 0x1000000
                samples[i] = val
            samples = (samples >> 8).astype(np.int16)  # normalize to 16-bit range
        elif bits == 32:
            samples = np.frombuffer(data, dtype=np.int32)
            samples = (samples >> 16).astype(np.int16)
        else:
            raise ValueError(f"Unsupported bit depth: {bits}")

        return samples, sample_rate, channels


def to_mono_float(samples: np.ndarray, channels: int) -> np.ndarray:
    """Convert interleaved multi-channel int16 to mono float [-1, 1]."""
    f = samples.astype(np.float64) / 32768.0
    if channels > 1:
        f = f.reshape(-1, channels).mean(axis=1)
    return f


def find_offset(tx: np.ndarray, rx: np.ndarray, max_offset_samples: int = 480000) -> tuple:
    """Find the time offset between TX and RX using cross-correlation.
    Returns (offset_samples, correlation_peak).
    Positive offset = RX is delayed relative to TX."""
    # Use a chunk from the middle of the shorter signal for speed
    ref_len = min(len(tx), len(rx), 48000 * 5)  # max 5 seconds for correlation
    mid_tx = len(tx) // 2
    tx_chunk = tx[mid_tx - ref_len//2 : mid_tx + ref_len//2]

    # Search window in RX
    search_start = max(0, mid_tx - max_offset_samples)
    search_end = min(len(rx), mid_tx + max_offset_samples + ref_len)
    rx_search = rx[search_start:search_end]

    if len(rx_search) < ref_len:
        return 0, 0.0

    # Normalized cross-correlation using FFT
    from scipy.signal import fftconvolve
    corr = fftconvolve(rx_search, tx_chunk[::-1], mode='valid')

    # Normalize
    tx_energy = np.sqrt(np.sum(tx_chunk ** 2))
    if tx_energy < 1e-10:
        return 0, 0.0

    # Running energy of rx
    rx_sq = rx_search ** 2
    rx_cumsum = np.cumsum(rx_sq)
    rx_window_energy = np.sqrt(
        rx_cumsum[ref_len-1:] - np.concatenate([[0], rx_cumsum[:len(rx_cumsum)-ref_len]])
    )
    rx_window_energy = np.maximum(rx_window_energy, 1e-10)

    corr_norm = corr[:len(rx_window_energy)] / (tx_energy * rx_window_energy)

    peak_idx = np.argmax(corr_norm)
    peak_val = corr_norm[peak_idx]

    # Convert to offset relative to TX
    offset = (search_start + peak_idx) - (mid_tx - ref_len // 2)

    return int(offset), float(peak_val)


def compute_snr(tx: np.ndarray, rx: np.ndarray) -> float:
    """Compute SNR in dB: signal power / (signal - received) power."""
    min_len = min(len(tx), len(rx))
    t = tx[:min_len]
    r = rx[:min_len]

    signal_power = np.mean(t ** 2)
    noise = r - t
    noise_power = np.mean(noise ** 2)

    if noise_power < 1e-20:
        return 120.0  # essentially perfect
    if signal_power < 1e-20:
        return -120.0

    return 10 * np.log10(signal_power / noise_power)


def detect_dropouts(rx: np.ndarray, sample_rate: int, threshold_db: float = -50.0) -> list:
    """Detect silence gaps (dropouts) in the RX signal.
    Returns list of (start_ms, duration_ms)."""
    threshold = 10 ** (threshold_db / 20)
    window = sample_rate // 100  # 10ms windows

    dropouts = []
    in_dropout = False
    dropout_start = 0

    for i in range(0, len(rx) - window, window):
        chunk = rx[i:i+window]
        rms = np.sqrt(np.mean(chunk ** 2))
        if rms < threshold:
            if not in_dropout:
                in_dropout = True
                dropout_start = i
        else:
            if in_dropout:
                in_dropout = False
                start_ms = dropout_start * 1000.0 / sample_rate
                dur_ms = (i - dropout_start) * 1000.0 / sample_rate
                if dur_ms > 5.0:  # ignore < 5ms gaps
                    dropouts.append((start_ms, dur_ms))

    return dropouts


def spectral_difference(tx: np.ndarray, rx: np.ndarray, sample_rate: int) -> dict:
    """Compare frequency spectrum of TX vs RX.
    Returns per-band energy differences in dB."""
    min_len = min(len(tx), len(rx))
    t = tx[:min_len]
    r = rx[:min_len]

    # FFT
    n = min(min_len, sample_rate * 10)  # max 10 seconds
    t_fft = np.abs(np.fft.rfft(t[:n]))
    r_fft = np.abs(np.fft.rfft(r[:n]))

    freqs = np.fft.rfftfreq(n, 1.0 / sample_rate)

    # Band analysis: sub-bass, bass, low-mid, mid, high-mid, presence, brilliance
    bands = {
        'sub_bass (20-60Hz)': (20, 60),
        'bass (60-250Hz)': (60, 250),
        'low_mid (250-500Hz)': (250, 500),
        'mid (500-2kHz)': (500, 2000),
        'high_mid (2-4kHz)': (2000, 4000),
        'presence (4-6kHz)': (4000, 6000),
        'brilliance (6-20kHz)': (6000, 20000),
    }

    result = {}
    for name, (lo, hi) in bands.items():
        mask = (freqs >= lo) & (freqs < hi)
        if not np.any(mask):
            continue
        t_energy = np.mean(t_fft[mask] ** 2)
        r_energy = np.mean(r_fft[mask] ** 2)
        if t_energy < 1e-20:
            result[name] = 0.0
        else:
            result[name] = 10 * np.log10(max(r_energy, 1e-20) / t_energy)

    return result


def main():
    parser = argparse.ArgumentParser(description='Compare TX vs RX audio quality')
    parser.add_argument('tx_wav', help='TX (transmitted) WAV file')
    parser.add_argument('rx_wav', help='RX (received) WAV file')
    parser.add_argument('--plot', action='store_true', help='Show plots')
    args = parser.parse_args()

    print(f"Loading TX: {args.tx_wav}")
    tx_samples, tx_rate, tx_ch = read_wav(args.tx_wav)
    tx = to_mono_float(tx_samples, tx_ch)

    print(f"Loading RX: {args.rx_wav}")
    rx_samples, rx_rate, rx_ch = read_wav(args.rx_wav)
    rx = to_mono_float(rx_samples, rx_ch)

    print(f"\n{'='*60}")
    print(f"TX: {len(tx)} samples ({len(tx)/tx_rate:.2f}s), {tx_rate}Hz, {tx_ch}ch")
    print(f"RX: {len(rx)} samples ({len(rx)/rx_rate:.2f}s), {rx_rate}Hz, {rx_ch}ch")

    if tx_rate != rx_rate:
        print(f"\n⚠ Sample rate mismatch! TX={tx_rate} RX={rx_rate}")
        print("  Resampling RX to match TX...")
        from scipy.signal import resample
        rx = resample(rx, int(len(rx) * tx_rate / rx_rate))

    # 1. Find timing offset
    print(f"\n{'─'*60}")
    print("Alignment (cross-correlation)...")
    offset, corr_peak = find_offset(tx, rx)
    offset_ms = offset * 1000.0 / tx_rate
    print(f"  Offset:      {offset} samples ({offset_ms:.1f} ms)")
    print(f"  Correlation: {corr_peak:.6f}")

    if corr_peak < 0.5:
        print("  ⚠ Low correlation — signals may not match (different audio?)")

    # Align signals
    if offset > 0:
        rx_aligned = rx[offset:]
        tx_aligned = tx[:len(rx_aligned)]
    else:
        tx_aligned = tx[-offset:]
        rx_aligned = rx[:len(tx_aligned)]

    min_len = min(len(tx_aligned), len(rx_aligned))
    tx_aligned = tx_aligned[:min_len]
    rx_aligned = rx_aligned[:min_len]

    # 2. SNR
    print(f"\n{'─'*60}")
    snr = compute_snr(tx_aligned, rx_aligned)
    print(f"  SNR:         {snr:.1f} dB")
    if snr > 60:
        print("  ✓ Excellent — near-transparent transmission")
    elif snr > 40:
        print("  ✓ Good — minor artifacts")
    elif snr > 20:
        print("  △ Fair — noticeable quality loss")
    else:
        print("  ✗ Poor — significant degradation")

    # 3. Sample drift (ASRC)
    print(f"\n{'─'*60}")
    drift = len(rx) - len(tx)
    drift_ppm = (drift * 1e6) / len(tx) if len(tx) > 0 else 0
    print(f"  Sample drift: {drift} samples ({drift_ppm:.1f} ppm)")
    if abs(drift_ppm) > 100:
        print("  ⚠ Large drift — check ASRC or clock sync")
    elif abs(drift_ppm) > 10:
        print("  △ Minor drift — ASRC is compensating")
    else:
        print("  ✓ Minimal drift")

    # 4. Dropouts
    print(f"\n{'─'*60}")
    dropouts = detect_dropouts(rx_aligned, tx_rate)
    print(f"  Dropouts:    {len(dropouts)}")
    if dropouts:
        for start, dur in dropouts[:10]:
            print(f"    @ {start:.0f}ms  ({dur:.1f}ms)")
        if len(dropouts) > 10:
            print(f"    ... and {len(dropouts)-10} more")
        print("  → Increase buffer (--buf-ms) or check WiFi stability")

    # 5. Spectral difference
    print(f"\n{'─'*60}")
    print("Spectral analysis (RX energy relative to TX):")
    spec_diff = spectral_difference(tx_aligned, rx_aligned, tx_rate)
    for band, diff_db in spec_diff.items():
        bar = "=" * int(min(abs(diff_db), 30))
        sign = "+" if diff_db > 0 else "-" if diff_db < 0 else " "
        print(f"  {band:30s} {sign}{abs(diff_db):5.1f} dB  {bar}")

    # 6. Error signal analysis
    print(f"\n{'─'*60}")
    noise = rx_aligned - tx_aligned
    noise_rms = np.sqrt(np.mean(noise ** 2))
    noise_peak = np.max(np.abs(noise))
    noise_db = 20 * np.log10(max(noise_rms, 1e-20))
    print(f"  Error RMS:   {noise_db:.1f} dBFS")
    print(f"  Error peak:  {20*np.log10(max(noise_peak, 1e-20)):.1f} dBFS")

    # 7. Recommendations
    print(f"\n{'='*60}")
    print("RECOMMENDATIONS:")
    recs = []
    if snr < 40:
        recs.append("- Increase jitter buffer target (--buf-ms)")
    if len(dropouts) > 5:
        recs.append("- Check WiFi signal strength or use wired connection")
        recs.append("- Enable FEC (--wifi-latency) for packet loss recovery")
    if abs(drift_ppm) > 50:
        recs.append("- Check sample rate clock accuracy (PTP/PLL)")
    if corr_peak < 0.8:
        recs.append("- Verify TX and RX are playing the same audio source")
    high_freq_loss = spec_diff.get('brilliance (6-20kHz)', 0)
    if high_freq_loss < -6:
        recs.append("- High-frequency loss detected — may indicate resampling issues")

    if recs:
        for r in recs:
            print(f"  {r}")
    else:
        print("  ✓ Audio quality looks good! No immediate improvements needed.")

    print()

    # Optional plots
    if args.plot:
        try:
            import matplotlib.pyplot as plt

            fig, axes = plt.subplots(4, 1, figsize=(14, 10))

            # Waveform comparison
            t_axis = np.arange(min_len) / tx_rate
            axes[0].plot(t_axis, tx_aligned, alpha=0.7, label='TX', linewidth=0.5)
            axes[0].plot(t_axis, rx_aligned, alpha=0.7, label='RX', linewidth=0.5)
            axes[0].set_title('Waveform Comparison')
            axes[0].set_xlabel('Time (s)')
            axes[0].legend()

            # Error signal
            axes[1].plot(t_axis, noise, color='red', linewidth=0.5)
            axes[1].set_title(f'Error Signal (SNR={snr:.1f}dB)')
            axes[1].set_xlabel('Time (s)')

            # Spectrogram of error
            axes[2].specgram(noise, NFFT=1024, Fs=tx_rate, cmap='hot')
            axes[2].set_title('Error Spectrogram')
            axes[2].set_ylabel('Frequency (Hz)')
            axes[2].set_xlabel('Time (s)')

            # Spectrum comparison
            n_fft = min(min_len, tx_rate * 5)
            freqs_plot = np.fft.rfftfreq(n_fft, 1.0 / tx_rate)
            tx_spec = 20 * np.log10(np.abs(np.fft.rfft(tx_aligned[:n_fft])) + 1e-10)
            rx_spec = 20 * np.log10(np.abs(np.fft.rfft(rx_aligned[:n_fft])) + 1e-10)
            axes[3].plot(freqs_plot, tx_spec, alpha=0.7, label='TX')
            axes[3].plot(freqs_plot, rx_spec, alpha=0.7, label='RX')
            axes[3].set_title('Frequency Spectrum')
            axes[3].set_xlabel('Frequency (Hz)')
            axes[3].set_ylabel('dB')
            axes[3].set_xscale('log')
            axes[3].set_xlim(20, tx_rate // 2)
            axes[3].legend()

            plt.tight_layout()
            plt.savefig('audio_compare.png', dpi=150)
            print(f"Plot saved to audio_compare.png")
            plt.show()
        except ImportError:
            print("Install matplotlib for plots: pip3 install matplotlib")


if __name__ == '__main__':
    main()
