#!/usr/bin/env bash
# install-web.sh — Install Soluna RPi web UI + update soluna-rx to use config file
# Usage: sudo bash deploy/rpi/install-web.sh [--channel jazz] [--relay HOST:PORT] [--alsa plughw:1,0]
set -e

if [[ $EUID -ne 0 ]]; then echo "Run with sudo"; exit 1; fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Defaults (detect current service config if possible)
RELAY="52.194.128.180:5100"
CHANNEL="jazz"
CODEC="pcm"
ALSA_DEVICE="plughw:1,0"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --channel) CHANNEL="$2"; shift 2 ;;
        --relay)   RELAY="$2";   shift 2 ;;
        --alsa)    ALSA_DEVICE="$2"; shift 2 ;;
        --codec)   CODEC="$2";   shift 2 ;;
        *) echo "Unknown: $1"; exit 1 ;;
    esac
done

echo "=== Installing Soluna Web UI ==="

# 1. Write config file
cat > /etc/soluna-rx.conf <<EOF
RELAY=$RELAY
CHANNEL=$CHANNEL
CODEC=$CODEC
ALSA_DEVICE=$ALSA_DEVICE
EOF
echo "Config: /etc/soluna-rx.conf"

# 2. Install start script
cp "$SCRIPT_DIR/soluna-start.sh" /usr/local/bin/soluna-start.sh
chmod +x /usr/local/bin/soluna-start.sh
echo "Launcher: /usr/local/bin/soluna-start.sh"

# 3. Install web UI script
cp "$SCRIPT_DIR/soluna-web.py" /usr/local/bin/soluna-web.py
chmod +x /usr/local/bin/soluna-web.py
echo "Web UI: /usr/local/bin/soluna-web.py"

# 4. Update soluna-rx.service to use the start script
cat > /etc/systemd/system/soluna-rx.service <<EOF
[Unit]
Description=Soluna Receiver
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/soluna-start.sh
Restart=always
RestartSec=5
KillMode=process

[Install]
WantedBy=multi-user.target
EOF
echo "Service: /etc/systemd/system/soluna-rx.service"

# 5. Install web UI service
cp "$SCRIPT_DIR/soluna-web.service" /etc/systemd/system/soluna-web.service
echo "Service: /etc/systemd/system/soluna-web.service"

# 6. Reload and restart
systemctl daemon-reload
systemctl enable soluna-rx soluna-web
systemctl restart soluna-rx
systemctl restart soluna-web

echo ""
echo "Done!"
echo "  Web UI:   http://$(hostname -I | awk '{print $1}'):8080"
echo "  WS ctrl:  ws://$(hostname -I | awk '{print $1}'):8400/ws"
echo ""
echo "  journalctl -fu soluna-rx    # audio logs"
echo "  journalctl -fu soluna-web   # web UI logs"
