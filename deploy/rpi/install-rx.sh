#!/usr/bin/env bash
# install-rx.sh — Install soluna-rx as a systemd service on Raspberry Pi / Linux
# Usage: sudo bash deploy/rpi/install-rx.sh [--device hw:1,0] [--group 239.69.0.1]
# SPDX-License-Identifier: MIT
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$(dirname "$(dirname "$SCRIPT_DIR")")/build/soluna-rx"
SERVICE_SRC="$SCRIPT_DIR/soluna-rx.service"
SERVICE_DEST="/etc/systemd/system/soluna-rx.service"
INSTALL_BIN="/usr/local/bin/soluna-rx"
SERVICE_ID="soluna-rx"

# ── Parse args ────────────────────────────────────────────────────────────────

ALSA_DEVICE="default"
MC_GROUP="239.69.0.1"
MC_PORT="5004"
CHANNELS="2"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device)   ALSA_DEVICE="$2"; shift 2 ;;
        --group)    MC_GROUP="$2";    shift 2 ;;
        --port)     MC_PORT="$2";     shift 2 ;;
        --channels) CHANNELS="$2";   shift 2 ;;
        --help)
            echo "Usage: sudo bash install-rx.sh [options]"
            echo "  --device <name>   ALSA device (default: default)"
            echo "  --group  <ip>     Multicast group (default: 239.69.0.1)"
            echo "  --port   <n>      UDP port (default: 5004)"
            echo "  --channels <n>    Channel count (default: 2)"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Checks ────────────────────────────────────────────────────────────────────

if [[ $EUID -ne 0 ]]; then
    echo "Error: run with sudo"
    exit 1
fi

if [[ ! -f "$BINARY" ]]; then
    echo "Error: soluna-rx not found at $BINARY"
    echo "Build first:"
    echo "  cmake -DCMAKE_BUILD_TYPE=Release -S . -B build"
    echo "  cmake --build build --target soluna-rx"
    exit 1
fi

# ── Create soluna user if needed ──────────────────────────────────────────────

if ! id -u soluna &>/dev/null; then
    useradd --system --no-create-home --shell /usr/sbin/nologin --groups audio soluna
    echo "Created system user: soluna"
fi

# ── Install binary ────────────────────────────────────────────────────────────

cp "$BINARY" "$INSTALL_BIN"
chmod +x "$INSTALL_BIN"
# Allow real-time scheduling without root
setcap cap_net_raw,cap_net_bind_service,cap_sys_nice=ep "$INSTALL_BIN" 2>/dev/null || true
echo "Installed: $INSTALL_BIN"

# ── Install service ───────────────────────────────────────────────────────────

sed \
    -e "s|SOLUNA_DEVICE=default|SOLUNA_DEVICE=$ALSA_DEVICE|g" \
    -e "s|SOLUNA_GROUP=239.69.0.1|SOLUNA_GROUP=$MC_GROUP|g" \
    -e "s|SOLUNA_PORT=5004|SOLUNA_PORT=$MC_PORT|g" \
    -e "s|SOLUNA_CHANNELS=2|SOLUNA_CHANNELS=$CHANNELS|g" \
    "$SERVICE_SRC" > "$SERVICE_DEST"
echo "Installed: $SERVICE_DEST"

# ── Enable and start ──────────────────────────────────────────────────────────

systemctl daemon-reload
systemctl enable "$SERVICE_ID"
systemctl restart "$SERVICE_ID"

echo ""
echo "Done! soluna-rx auto-starts on boot."
echo "  Group:    $MC_GROUP:$MC_PORT"
echo "  ALSA:     $ALSA_DEVICE  (${CHANNELS}ch)"
echo ""
echo "  systemctl status $SERVICE_ID     # status"
echo "  journalctl -fu $SERVICE_ID       # logs"
echo "  systemctl stop $SERVICE_ID       # stop"
echo "  systemctl disable $SERVICE_ID    # uninstall"
