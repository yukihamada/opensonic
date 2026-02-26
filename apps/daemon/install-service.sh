#!/usr/bin/env bash
# install-service.sh — Install solunad as a macOS LaunchAgent
# Usage: bash apps/daemon/install-service.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/../../build/solunad"
PLIST="$SCRIPT_DIR/io.soluna.daemon.plist"
LAUNCH_AGENTS="$HOME/Library/LaunchAgents"
SERVICE_ID="io.soluna.daemon"

if [ ! -f "$BINARY" ]; then
    echo "Error: solunad not found at $BINARY"
    echo "Build first: cmake --build build"
    exit 1
fi

echo "Installing solunad to /usr/local/bin..."
sudo cp "$BINARY" /usr/local/bin/solunad
sudo chmod +x /usr/local/bin/solunad

mkdir -p "$LAUNCH_AGENTS"
echo "Installing LaunchAgent plist..."
cp "$PLIST" "$LAUNCH_AGENTS/$SERVICE_ID.plist"

# Unload existing service if running
launchctl bootout "gui/$UID/$SERVICE_ID" 2>/dev/null || true

echo "Loading LaunchAgent..."
launchctl bootstrap "gui/$UID" "$LAUNCH_AGENTS/$SERVICE_ID.plist"

echo ""
echo "Done! solunad will start automatically on login."
echo "Logs: /tmp/solunad.log"
echo ""
echo "Manage with:"
echo "  launchctl print gui/$UID/$SERVICE_ID"
echo "  launchctl stop  gui/$UID/$SERVICE_ID"
echo "  launchctl start gui/$UID/$SERVICE_ID"
echo "  launchctl bootout gui/$UID/$SERVICE_ID  # uninstall"
