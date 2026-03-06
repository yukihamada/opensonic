#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────
# Soluna Mac Installer — ワンコマンドで全コンポーネントをセットアップ
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/yukihamada/opensonic/master/scripts/install-mac.sh | bash
#   # or
#   bash scripts/install-mac.sh
#
# Components:
#   1. solunad        — ネットワークオーディオデーモン
#   2. Soluna.driver   — CoreAudio 仮想デバイス
#   3. LaunchAgent     — ログイン時自動起動
#   4. solctl          — CLI コントロールツール
#
# SPDX-License-Identifier: MIT
# ──────────────────────────────────────────────────────────────
set -euo pipefail

# ── Colors ───────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NC='\033[0m'

info()  { echo -e "${BLUE}==>${NC} ${BOLD}$*${NC}"; }
ok()    { echo -e "${GREEN}  ✓${NC} $*"; }
warn()  { echo -e "${YELLOW}  !${NC} $*"; }
fail()  { echo -e "${RED}  ✗ $*${NC}"; exit 1; }

# ── Banner ───────────────────────────────────────────────────
echo ""
echo -e "${BOLD}  ◈ Soluna Installer${NC}"
echo -e "  Mac のシステム音声を、どこでも鳴らす"
echo ""

# ── Pre-flight checks ───────────────────────────────────────
[[ "$(uname)" == "Darwin" ]] || fail "macOS only"
command -v cmake >/dev/null 2>&1 || fail "cmake not found. Install: brew install cmake"
command -v git   >/dev/null 2>&1 || fail "git not found. Install: xcode-select --install"

# ── Determine source directory ───────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/../CMakeLists.txt" ]]; then
    SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
else
    # Running via curl — clone the repo
    CLONE_DIR="/tmp/soluna-install-$$"
    info "Cloning opensonic..."
    git clone --depth 1 https://github.com/yukihamada/opensonic.git "$CLONE_DIR" 2>/dev/null
    SRC_DIR="$CLONE_DIR"
    trap "rm -rf '$CLONE_DIR'" EXIT
fi

BUILD_DIR="$SRC_DIR/build"
INSTALL_DIR="$HOME/.local/bin"
LAUNCH_AGENTS="$HOME/Library/LaunchAgents"
SERVICE_ID="io.soluna.daemon"
DRIVER_DEST="/Library/Audio/Plug-Ins/HAL/Soluna.driver"

# ── Step 1: Build ────────────────────────────────────────────
info "Building Soluna (Release)..."
cmake -B "$BUILD_DIR" -S "$SRC_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
    2>/dev/null

NPROC=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
cmake --build "$BUILD_DIR" -j"$NPROC" 2>&1 | tail -3

[[ -f "$BUILD_DIR/solunad" ]] || fail "Build failed: solunad not found"
ok "solunad built"

[[ -f "$BUILD_DIR/solctl" ]] && ok "solctl built"

# ── Step 2: Install binaries ────────────────────────────────
info "Installing binaries to $INSTALL_DIR..."
mkdir -p "$INSTALL_DIR"
cp "$BUILD_DIR/solunad" "$INSTALL_DIR/solunad"
chmod +x "$INSTALL_DIR/solunad"
ok "solunad → $INSTALL_DIR/solunad"

if [[ -f "$BUILD_DIR/solctl" ]]; then
    cp "$BUILD_DIR/solctl" "$INSTALL_DIR/solctl"
    chmod +x "$INSTALL_DIR/solctl"
    ok "solctl → $INSTALL_DIR/solctl"
fi

# Add to PATH if not already there
if ! echo "$PATH" | grep -q "$INSTALL_DIR"; then
    SHELL_RC=""
    if [[ -f "$HOME/.zshrc" ]]; then
        SHELL_RC="$HOME/.zshrc"
    elif [[ -f "$HOME/.bashrc" ]]; then
        SHELL_RC="$HOME/.bashrc"
    fi
    if [[ -n "$SHELL_RC" ]] && ! grep -q "$INSTALL_DIR" "$SHELL_RC" 2>/dev/null; then
        echo "export PATH=\"$INSTALL_DIR:\$PATH\"" >> "$SHELL_RC"
        ok "Added $INSTALL_DIR to PATH ($SHELL_RC)"
    fi
fi

# ── Step 3: Install audio driver ─────────────────────────────
DRIVER_SRC="$BUILD_DIR/apps/plugin/Soluna.driver"
if [[ -d "$DRIVER_SRC" ]]; then
    info "Installing Soluna audio driver..."
    echo "  (sudo required for /Library/Audio/Plug-Ins/HAL/)"

    # Sign
    codesign --force --sign - --deep "$DRIVER_SRC" 2>/dev/null || true

    # Install
    sudo rm -rf "$DRIVER_DEST"
    sudo cp -r "$DRIVER_SRC" "$DRIVER_DEST"
    sudo chown -R root:wheel "$DRIVER_DEST"
    sudo chmod -R 755 "$DRIVER_DEST"
    ok "Soluna.driver → $DRIVER_DEST"

    # Restart coreaudiod
    info "Restarting coreaudiod..."
    sudo killall coreaudiod 2>/dev/null || true
    sleep 2
    ok "coreaudiod restarted"
else
    warn "Soluna.driver not built (skipped)"
    warn "Build manually: cmake --build build --target Soluna"
fi

# ── Step 4: Install LaunchAgent ──────────────────────────────
info "Installing LaunchAgent (auto-start on login)..."

# Stop existing service
launchctl bootout "gui/$UID/$SERVICE_ID" 2>/dev/null || true

mkdir -p "$LAUNCH_AGENTS"
cat > "$LAUNCH_AGENTS/$SERVICE_ID.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
    "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$SERVICE_ID</string>
    <key>ProgramArguments</key>
    <array>
        <string>$INSTALL_DIR/solunad</string>
        <string>--tx</string>
        <string>--device</string>
        <string>soluna</string>
        <string>--speaker</string>
        <string>auto</string>
        <string>--channels</string>
        <string>2</string>
        <string>--low-latency</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/tmp/solunad.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/solunad.log</string>
</dict>
</plist>
PLIST

launchctl bootstrap "gui/$UID" "$LAUNCH_AGENTS/$SERVICE_ID.plist"
ok "LaunchAgent registered (auto-start on login)"

# ── Step 5: Verify ───────────────────────────────────────────
info "Verifying installation..."
sleep 2

if pgrep -q solunad; then
    ok "solunad is running (PID $(pgrep solunad | head -1))"
else
    warn "solunad not yet running — check /tmp/solunad.log"
fi

# Check if Soluna device is visible
if system_profiler SPAudioDataType 2>/dev/null | grep -q "Soluna"; then
    ok "Soluna audio device detected"
else
    warn "Soluna audio device not yet visible"
    warn "Go to System Settings > Privacy & Security > allow the driver, then:"
    warn "  sudo killall coreaudiod"
fi

# ── Done ─────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}  Installation complete!${NC}"
echo ""
echo "  Next steps:"
echo "    1. System Settings → Sound → Output → Select ${BOLD}Soluna${NC}"
echo "    2. Open ${BOLD}http://localhost:8400${NC} for the dashboard"
echo ""
echo "  Commands:"
echo "    solunad --help              # usage"
echo "    solctl status               # daemon status"
echo "    launchctl stop gui/$UID/$SERVICE_ID   # stop"
echo "    launchctl start gui/$UID/$SERVICE_ID  # start"
echo "    tail -f /tmp/solunad.log    # logs"
echo ""
echo "  Uninstall:"
echo "    bash $SRC_DIR/scripts/uninstall-mac.sh"
echo ""
