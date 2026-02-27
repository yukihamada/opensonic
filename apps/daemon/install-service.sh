#!/usr/bin/env bash
# install-service.sh — Install solunad as a macOS LaunchAgent (no sudo required)
# Usage: bash apps/daemon/install-service.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/../../build/solunad"
PLIST="$SCRIPT_DIR/io.soluna.daemon.plist"
LAUNCH_AGENTS="$HOME/Library/LaunchAgents"
SERVICE_ID="io.soluna.daemon"
INSTALL_DIR="$HOME/.local/bin"

if [ ! -f "$BINARY" ]; then
    echo "Error: solunad not found at $BINARY"
    echo "Build first: cmake --build build"
    exit 1
fi

# Install binary to ~/.local/bin (no sudo needed)
mkdir -p "$INSTALL_DIR"
cp "$BINARY" "$INSTALL_DIR/solunad"
chmod +x "$INSTALL_DIR/solunad"
echo "Installed: $INSTALL_DIR/solunad"

# Patch ProgramArguments in plist to use the local path
PLIST_DEST="$LAUNCH_AGENTS/$SERVICE_ID.plist"
mkdir -p "$LAUNCH_AGENTS"
sed "s|/usr/local/bin/solunad|$INSTALL_DIR/solunad|g" "$PLIST" > "$PLIST_DEST"
echo "Installed plist: $PLIST_DEST"

# Unload existing service if running
launchctl bootout "gui/$UID/$SERVICE_ID" 2>/dev/null || true

# Register with launchd
launchctl bootstrap "gui/$UID" "$PLIST_DEST"

echo ""
echo "Done! solunad auto-starts on login."
echo "Logs: /tmp/solunad.log"
echo ""
echo "  launchctl print gui/$UID/$SERVICE_ID   # status"
echo "  launchctl stop  gui/$UID/$SERVICE_ID   # stop"
echo "  launchctl start gui/$UID/$SERVICE_ID   # start"
echo "  launchctl bootout gui/$UID/$SERVICE_ID # uninstall"
