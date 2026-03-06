#!/usr/bin/env python3
"""
auto_optimize.py — Automatic audio quality optimization for Soluna

Monitors RX quality metrics from Raspi (soluna-rx --metrics) or iPhone,
and auto-adjusts solunad parameters via WebSocket to converge on optimal settings.

Architecture:
    [soluna-rx --metrics]  →  stderr JSON  →  [auto_optimize.py]  →  WebSocket  →  [solunad]
                                                     ↑
    [iPhone metrics]       →  (future)     ──────────┘

Usage:
    # Raspi mode (SSH to Raspi, reads metrics, adjusts solunad locally)
    python3 auto_optimize.py --raspi user@raspi.local --solunad-host localhost

    # Local test mode (reads metrics from stdin)
    soluna-rx --metrics 2>&1 | python3 auto_optimize.py --stdin --solunad-host localhost

    # WAV comparison mode (one-shot calibration)
    python3 auto_optimize.py --calibrate --tx tx.wav --rx rx.wav --solunad-host localhost

Requirements:
    pip3 install websocket-client  (or websockets)
"""

import sys
import json
import time
import argparse
import subprocess
import threading


class SolunadController:
    """Controls solunad parameters via WebSocket."""

    def __init__(self, host: str, port: int = 8400):
        self.url = f"ws://{host}:{port}/ws"
        self.ws = None
        self._connect()

    def _connect(self):
        try:
            import websocket
            self.ws = websocket.create_connection(self.url, timeout=5)
            print(f"[ctrl] Connected to solunad at {self.url}")
        except Exception as e:
            print(f"[ctrl] Cannot connect to {self.url}: {e}")
            self.ws = None

    def send(self, cmd: dict):
        if not self.ws:
            self._connect()
        if not self.ws:
            return None
        try:
            # solunad parser expects no spaces: "command":"xxx" not "command": "xxx"
            self.ws.send(json.dumps(cmd, separators=(',', ':')))
            resp = self.ws.recv()
            return json.loads(resp) if resp else None
        except Exception as e:
            print(f"[ctrl] WebSocket error: {e}")
            self.ws = None
            return None

    def set_buffer(self, ms: int):
        return self.send({"command": "rx.set_buffer", "ms": max(5, min(2000, ms))})

    def set_noise(self, **kwargs):
        cmd = {"command": "noise.set"}
        cmd.update(kwargs)
        return self.send(cmd)

    def set_wifi(self, **kwargs):
        cmd = {"command": "wifi.set"}
        cmd.update(kwargs)
        return self.send(cmd)

    def get_stats(self):
        return self.send({"command": "rx.stats"})

    def get_noise(self):
        return self.send({"command": "noise.get"})

    def close(self):
        if self.ws:
            self.ws.close()


class Optimizer:
    """Convergence algorithm for audio quality optimization."""

    def __init__(self, ctrl: SolunadController):
        self.ctrl = ctrl

        # Current parameter state
        self.buf_ms = 20         # rx buffer target
        self.sigma = 6.0         # click detection threshold
        self.cf_frames = 16      # crossfade frames
        self.cf_thresh = 0.02    # crossfade threshold
        self.fec = True
        self.nack = True
        self.wsola_plc = True

        # Convergence tracking
        self.history = []        # last N metrics
        self.stable_count = 0    # consecutive good intervals
        self.adjustments = 0

    def process_metrics(self, m: dict):
        """Process one metrics JSON and decide adjustments."""
        self.history.append(m)
        if len(self.history) > 20:
            self.history.pop(0)

        loss = m.get("loss_pct", 0)
        clicks = m.get("clicks", 0)
        dropouts = m.get("dropouts", 0)
        underruns = m.get("underruns", 0)
        rms_db = m.get("rms_db", -120)

        # Score: 0=perfect, higher=worse
        score = 0
        reasons = []

        if loss > 1.0:
            score += 30
            reasons.append(f"packet_loss={loss:.1f}%")
        elif loss > 0.1:
            score += 10
            reasons.append(f"packet_loss={loss:.2f}%")

        # Clicks: only count if significantly less than packet count
        # (if clicks ≈ pkts_rx, it's just packet boundaries, not real glitches)
        pkts = m.get("pkts_rx", 1)
        click_rate = clicks / max(pkts, 1)
        if click_rate < 0.5:  # real clicks (not packet boundaries)
            if clicks > 5:
                score += 20
                reasons.append(f"clicks={clicks}")
            elif clicks > 0:
                score += 5
                reasons.append(f"clicks={clicks}")

        if dropouts > 0:
            score += 25
            reasons.append(f"dropouts={dropouts}")

        if underruns > 0:
            score += 15
            reasons.append(f"underruns={underruns}")

        # Log
        status = "GOOD" if score == 0 else "FAIR" if score < 15 else "POOR"
        reason_str = ", ".join(reasons) if reasons else "clean"
        print(f"[opt] {status} (score={score}) {reason_str}  "
              f"buf={self.buf_ms}ms σ={self.sigma:.1f} cf={self.cf_frames}",
              flush=True)

        # Decide actions
        if score == 0:
            self.stable_count += 1
            if self.stable_count >= 6:  # 30s stable → try reducing buffer
                self._try_relax()
        else:
            self.stable_count = 0
            self._apply_fixes(loss, clicks, dropouts, underruns, score)

    def _apply_fixes(self, loss, clicks, dropouts, underruns, score):
        """Apply parameter adjustments based on detected issues."""
        changed = False

        # 1. Packet loss → increase buffer + ensure FEC/NACK
        if loss > 0.5:
            new_buf = min(self.buf_ms + 5, 200)
            if new_buf != self.buf_ms:
                self.buf_ms = new_buf
                self.ctrl.set_buffer(self.buf_ms)
                print(f"  → buffer: {self.buf_ms}ms (packet loss)")
                changed = True

            if not self.fec:
                self.fec = True
                self.ctrl.set_wifi(fec=True)
                print(f"  → FEC enabled")
                changed = True

            if not self.nack:
                self.nack = True
                self.ctrl.set_wifi(nack=True)
                print(f"  → NACK enabled")
                changed = True

        # 2. Clicks → more aggressive noise repair
        if clicks > 0:
            if self.sigma > 2.0:
                self.sigma = max(self.sigma - 1.0, 2.0)
                self.ctrl.set_noise(sigma=self.sigma)
                print(f"  → sigma: {self.sigma:.1f} (clicks)")
                changed = True

            if self.cf_frames < 64:
                self.cf_frames = min(self.cf_frames + 8, 64)
                self.ctrl.set_noise(crossfade_frames=self.cf_frames)
                print(f"  → crossfade: {self.cf_frames} frames (clicks)")
                changed = True

            if self.cf_thresh > 0.005:
                self.cf_thresh = max(self.cf_thresh - 0.005, 0.005)
                self.ctrl.set_noise(crossfade_thresh=self.cf_thresh)
                print(f"  → cf_thresh: {self.cf_thresh:.3f} (clicks)")
                changed = True

        # 3. Dropouts → increase buffer significantly
        if dropouts > 0:
            new_buf = min(self.buf_ms + 10, 200)
            if new_buf != self.buf_ms:
                self.buf_ms = new_buf
                self.ctrl.set_buffer(self.buf_ms)
                print(f"  → buffer: {self.buf_ms}ms (dropouts)")
                changed = True

            if not self.wsola_plc:
                self.wsola_plc = True
                self.ctrl.set_wifi(wsola_plc=True)
                print(f"  → WSOLA PLC enabled")
                changed = True

        # 4. Underruns → increase buffer
        if underruns > 0:
            new_buf = min(self.buf_ms + 3, 200)
            if new_buf != self.buf_ms:
                self.buf_ms = new_buf
                self.ctrl.set_buffer(self.buf_ms)
                print(f"  → buffer: {self.buf_ms}ms (underruns)")
                changed = True

        # Detect persistent high packet loss (WiFi multicast problem)
        if loss > 30 and len(self.history) >= 5:
            recent_loss = [h.get("loss_pct", 0) for h in self.history[-5:]]
            if all(l > 30 for l in recent_loss):
                print("  ⚠ Persistent >30% packet loss detected!")
                print("    WiFi multicast is fundamentally unreliable.")
                print("    Recommendations:")
                print("    1. Use wired (Ethernet) connection if possible")
                print("    2. Switch to Peer Relay mode (P2P unicast)")
                print("    3. Move Raspi closer to WiFi AP")
                print("    4. Use 5GHz WiFi band (less interference)")
                self._high_loss_warned = True

        if changed:
            self.adjustments += 1

    def _try_relax(self):
        """Try to reduce parameters when stable (lower latency)."""
        changed = False

        # Decrease buffer (minimum viable)
        if self.buf_ms > 10:
            self.buf_ms = max(self.buf_ms - 2, 10)
            self.ctrl.set_buffer(self.buf_ms)
            print(f"  ↓ buffer: {self.buf_ms}ms (stable, reducing latency)")
            changed = True

        # Relax click detection (less CPU)
        if self.sigma < 6.0:
            self.sigma = min(self.sigma + 0.5, 6.0)
            self.ctrl.set_noise(sigma=self.sigma)
            print(f"  ↓ sigma: {self.sigma:.1f} (relaxing)")
            changed = True

        if self.cf_frames > 16:
            self.cf_frames = max(self.cf_frames - 4, 16)
            self.ctrl.set_noise(crossfade_frames=self.cf_frames)
            print(f"  ↓ crossfade: {self.cf_frames} (relaxing)")
            changed = True

        if changed:
            self.stable_count = 0  # reset stability counter after change
            self.adjustments += 1


def run_raspi_mode(args):
    """SSH to Raspi and monitor soluna-rx --metrics output."""
    ctrl = SolunadController(args.solunad_host, args.solunad_port)
    opt = Optimizer(ctrl)

    cmd = [
        "ssh", args.raspi,
        f"soluna-rx --metrics --metrics-interval {args.interval}"
    ]
    if args.peer:
        cmd[-1] += f" --peer {args.peer}"
    elif args.group:
        cmd[-1] += f" --group {args.group}"
    if args.port:
        cmd[-1] += f" --port {args.port}"

    print(f"[opt] Starting: {' '.join(cmd)}")
    proc = subprocess.Popen(cmd, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL,
                            text=True, bufsize=1)

    try:
        for line in proc.stderr:
            line = line.strip()
            if not line:
                continue
            if line.startswith("{") and '"type":"metrics"' in line:
                try:
                    m = json.loads(line)
                    opt.process_metrics(m)
                except json.JSONDecodeError:
                    pass
            else:
                # Non-JSON output from soluna-rx
                print(f"[rx] {line}")
    except KeyboardInterrupt:
        print(f"\n[opt] Stopped. {opt.adjustments} total adjustments made.")
    finally:
        proc.terminate()
        ctrl.close()


def run_stdin_mode(args):
    """Read metrics from stdin (piped from soluna-rx --metrics)."""
    ctrl = SolunadController(args.solunad_host, args.solunad_port)
    opt = Optimizer(ctrl)

    print("[opt] Reading metrics from stdin...")
    try:
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            if line.startswith("{") and '"type":"metrics"' in line:
                try:
                    m = json.loads(line)
                    opt.process_metrics(m)
                except json.JSONDecodeError:
                    pass
    except KeyboardInterrupt:
        print(f"\n[opt] Stopped. {opt.adjustments} total adjustments made.")
    finally:
        ctrl.close()


def run_calibrate_mode(args):
    """One-shot WAV comparison + auto-adjustment."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "audio_compare",
        args.compare_script or "tools/audio_compare.py"
    )
    if not spec:
        print("Cannot find audio_compare.py")
        return

    # Use audio_compare functions directly
    sys.path.insert(0, "tools")
    from audio_compare import read_wav, to_mono_float, find_offset, compute_snr, detect_dropouts

    ctrl = SolunadController(args.solunad_host, args.solunad_port)

    print(f"[cal] Loading TX: {args.tx}")
    tx_samples, tx_rate, tx_ch = read_wav(args.tx)
    tx = to_mono_float(tx_samples, tx_ch)

    print(f"[cal] Loading RX: {args.rx}")
    rx_samples, rx_rate, rx_ch = read_wav(args.rx)
    rx = to_mono_float(rx_samples, rx_ch)

    # Analyze
    offset, corr_peak = find_offset(tx, rx)
    offset_ms = offset * 1000.0 / tx_rate

    if offset > 0:
        rx_a = rx[offset:]
        tx_a = tx[:len(rx_a)]
    else:
        tx_a = tx[-offset:]
        rx_a = rx[:len(tx_a)]
    min_len = min(len(tx_a), len(rx_a))
    tx_a, rx_a = tx_a[:min_len], rx_a[:min_len]

    snr = compute_snr(tx_a, rx_a)
    dropouts = detect_dropouts(rx_a, tx_rate)
    drift_ppm = (len(rx) - len(tx)) * 1e6 / len(tx) if len(tx) > 0 else 0

    print(f"\n[cal] Results:")
    print(f"  Offset: {offset_ms:.1f}ms, Correlation: {corr_peak:.4f}")
    print(f"  SNR: {snr:.1f}dB, Dropouts: {len(dropouts)}, Drift: {drift_ppm:.0f}ppm")

    # Auto-adjust based on results
    changes = []

    if snr < 30:
        new_buf = 50
        ctrl.set_buffer(new_buf)
        changes.append(f"buffer → {new_buf}ms (low SNR)")
        ctrl.set_noise(sigma=3.0, crossfade_frames=48, crossfade_thresh=0.008)
        changes.append("aggressive noise repair (σ=3, cf=48)")

    elif snr < 45:
        new_buf = 30
        ctrl.set_buffer(new_buf)
        changes.append(f"buffer → {new_buf}ms (moderate SNR)")
        ctrl.set_noise(sigma=4.0, crossfade_frames=32, crossfade_thresh=0.012)
        changes.append("moderate noise repair (σ=4, cf=32)")

    else:
        # Good quality — try lower latency
        new_buf = 15
        ctrl.set_buffer(new_buf)
        changes.append(f"buffer → {new_buf}ms (good SNR, low latency)")

    if len(dropouts) > 5:
        ctrl.set_wifi(fec=True, nack=True, wsola_plc=True, dup_send=True)
        changes.append("WiFi resilience: all features ON")
    elif len(dropouts) > 0:
        ctrl.set_wifi(fec=True, nack=True, wsola_plc=True)
        changes.append("WiFi resilience: FEC+NACK+PLC ON")

    if abs(drift_ppm) > 100:
        ctrl.set_wifi(adaptive_jitter=True)
        changes.append("adaptive jitter ON (clock drift)")

    print(f"\n[cal] Applied {len(changes)} adjustments:")
    for c in changes:
        print(f"  → {c}")

    ctrl.close()


def main():
    parser = argparse.ArgumentParser(
        description='Auto-optimize Soluna audio quality')

    parser.add_argument('--solunad-host', default='localhost',
                        help='solunad WebSocket host (default: localhost)')
    parser.add_argument('--solunad-port', type=int, default=8400,
                        help='solunad WebSocket port (default: 8400)')
    parser.add_argument('--interval', type=int, default=5,
                        help='Metrics interval in seconds (default: 5)')
    parser.add_argument('--group', help='Multicast group for soluna-rx')
    parser.add_argument('--port', type=int, help='RTP port for soluna-rx')
    parser.add_argument('--peer', metavar='HOST:PORT',
                        help='P2P relay address for soluna-rx (e.g., 192.168.0.194:5099)')

    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--raspi', metavar='USER@HOST',
                      help='SSH to Raspi and monitor soluna-rx metrics')
    mode.add_argument('--stdin', action='store_true',
                      help='Read metrics from stdin')
    mode.add_argument('--calibrate', action='store_true',
                      help='One-shot WAV comparison calibration')

    parser.add_argument('--tx', help='TX WAV file (for --calibrate)')
    parser.add_argument('--rx', help='RX WAV file (for --calibrate)')
    parser.add_argument('--compare-script', help='Path to audio_compare.py')

    args = parser.parse_args()

    if args.calibrate and (not args.tx or not args.rx):
        parser.error("--calibrate requires --tx and --rx")

    if args.raspi:
        run_raspi_mode(args)
    elif args.stdin:
        run_stdin_mode(args)
    elif args.calibrate:
        run_calibrate_mode(args)


if __name__ == '__main__':
    main()
