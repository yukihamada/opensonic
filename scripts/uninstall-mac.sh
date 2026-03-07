#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────
# Soluna Mac Uninstaller — クリーンに全コンポーネントを削除
#
# Usage:
#   bash scripts/uninstall-mac.sh
#
# SPDX-License-Identifier: MIT
# ──────────────────────────────────────────────────────────────
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NC='\033[0m'

info()  { echo -e "${BLUE}==>${NC} ${BOLD}$*${NC}"; }
ok()    { echo -e "${GREEN}  ✓${NC} $*"; }
warn()  { echo -e "${YELLOW}  !${NC} $*"; }

SERVICE_ID="io.soluna.daemon"
INSTALL_DIR="$HOME/.local/bin"
LAUNCH_AGENTS="$HOME/Library/LaunchAgents"
DRIVER_DEST="/Library/Audio/Plug-Ins/HAL/Soluna.driver"
APP_DEST="/Applications/Soluna.app"
SHM_DIR="/private/var/db/soluna"

echo ""
echo -e "${BOLD}  ◈ Soluna Uninstaller${NC}"
echo ""

# ── Step 1: Stop daemon ──────────────────────────────────────
info "Stopping solunad..."
launchctl bootout "gui/$UID/$SERVICE_ID" 2>/dev/null && ok "LaunchAgent removed" || warn "LaunchAgent not registered"
pkill -f solunad 2>/dev/null && ok "solunad stopped" || warn "solunad not running"

# ── Step 2: Remove LaunchAgent plist ─────────────────────────
info "Removing LaunchAgent..."
PLIST="$LAUNCH_AGENTS/$SERVICE_ID.plist"
if [[ -f "$PLIST" ]]; then
    rm -f "$PLIST"
    ok "Removed $PLIST"
else
    warn "Plist not found (already removed)"
fi

# ── Step 3: Remove binaries ──────────────────────────────────
info "Removing binaries..."
for bin in solunad solctl; do
    if [[ -f "$INSTALL_DIR/$bin" ]]; then
        rm -f "$INSTALL_DIR/$bin"
        ok "Removed $INSTALL_DIR/$bin"
    fi
done

# ── Step 4: Remove Soluna.app ──────────────────────────────
info "Removing Soluna.app..."
if [[ -d "$APP_DEST" ]]; then
    rm -rf "$APP_DEST"
    ok "Removed $APP_DEST"
else
    warn "Soluna.app not found (already removed)"
fi

# ── Step 5: Remove audio driver ──────────────────────────────
info "Removing audio driver..."
if [[ -d "$DRIVER_DEST" ]]; then
    echo "  (sudo required)"
    sudo rm -rf "$DRIVER_DEST"
    ok "Removed $DRIVER_DEST"

    info "Restarting coreaudiod..."
    sudo killall coreaudiod 2>/dev/null || true
    sleep 1
    ok "coreaudiod restarted"
else
    warn "Driver not found (already removed)"
fi

# ── Step 6: Remove shared memory directory ───────────────────
info "Removing shared memory directory..."
if [[ -d "$SHM_DIR" ]]; then
    sudo rm -rf "$SHM_DIR"
    ok "Removed $SHM_DIR"
else
    warn "Shared memory directory not found"
fi

# ── Step 7: Remove config ────────────────────────────────────
info "Removing config..."
CONFIG_DIR="$HOME/.config/solunad"
if [[ -d "$CONFIG_DIR" ]]; then
    rm -rf "$CONFIG_DIR"
    ok "Removed $CONFIG_DIR"
else
    warn "No config directory found"
fi

# ── Step 8: Remove logs ──────────────────────────────────────
info "Removing logs..."
rm -f /tmp/solunad.log 2>/dev/null && ok "Removed /tmp/solunad.log" || true

# ── Done ─────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}  Soluna has been completely uninstalled.${NC}"
echo ""
echo "  Note: System Settings → Sound → Output should no longer show 'Soluna'."
echo "  If it still appears, restart your Mac."
echo ""
