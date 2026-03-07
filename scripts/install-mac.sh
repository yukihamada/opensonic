#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────
# Soluna Mac Installer — ワンコマンドで Soluna.app + Soluna.driver をセットアップ
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/yukihamada/opensonic/master/scripts/install-mac.sh | bash
#   # or
#   bash scripts/install-mac.sh
#   bash scripts/install-mac.sh --headless   # solunad + LaunchAgent もインストール
#
# Components:
#   1. Soluna.driver   — CoreAudio 仮想デバイス (HAL プラグイン)
#   2. Soluna.app      — GUI アプリ (Audio TX / Mic TX / WAN P2P)
#   3. (--headless のみ) solunad + LaunchAgent
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

# ── Parse flags ─────────────────────────────────────────────
HEADLESS=false
for arg in "$@"; do
    case "$arg" in
        --headless) HEADLESS=true ;;
    esac
done

# ── Banner ───────────────────────────────────────────────────
echo ""
echo -e "${BOLD}  ◈ Soluna Installer${NC}"
echo -e "  Mac のシステム音声を、どこでも鳴らす"
echo ""

# ── Pre-flight checks ───────────────────────────────────────
[[ "$(uname)" == "Darwin" ]] || fail "macOS only"

# macOS version check (minimum 13.0 Ventura)
MACOS_VER=$(sw_vers -productVersion 2>/dev/null || echo "0")
MACOS_MAJOR=$(echo "$MACOS_VER" | cut -d. -f1)
if [[ "$MACOS_MAJOR" -lt 13 ]]; then
    fail "macOS 13 (Ventura) or later required. Current: $MACOS_VER"
fi
ok "macOS $MACOS_VER"

command -v cmake      >/dev/null 2>&1 || fail "cmake not found. Install: brew install cmake"
command -v xcodebuild >/dev/null 2>&1 || fail "xcodebuild not found. Install: xcode-select --install"
command -v git        >/dev/null 2>&1 || fail "git not found. Install: xcode-select --install"

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

BUILD_DIR="$SRC_DIR/build-mac"
DRIVER_DEST="/Library/Audio/Plug-Ins/HAL/Soluna.driver"
SHM_DIR="/private/var/db/soluna"
NPROC=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

# ── Step 1: cmake — libsoluna_core.a + Soluna.driver ────────
info "Building Soluna core + driver (cmake)..."
CMAKE_LOG="$BUILD_DIR/cmake.log"
mkdir -p "$BUILD_DIR"

if ! cmake -B "$BUILD_DIR" -S "$SRC_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
    > "$CMAKE_LOG" 2>&1; then
    echo ""
    tail -20 "$CMAKE_LOG"
    fail "cmake configure failed. Full log: $CMAKE_LOG"
fi

if ! cmake --build "$BUILD_DIR" -j"$NPROC" >> "$CMAKE_LOG" 2>&1; then
    echo ""
    tail -20 "$CMAKE_LOG"
    fail "cmake build failed. Full log: $CMAKE_LOG"
fi

DRIVER_SRC="$BUILD_DIR/apps/plugin/Soluna.driver"
[[ -d "$DRIVER_SRC" ]] || fail "Build failed: Soluna.driver not found in $BUILD_DIR. Check: $CMAKE_LOG"
ok "Soluna.driver built"

# ── Step 2: xcodebuild — Soluna.app ─────────────────────────
XCODEPROJ="$SRC_DIR/apps/mac-rx/SolunaReceiverMac.xcodeproj"
APP_OUTPUT="$BUILD_DIR/app-output"

if [[ -d "$XCODEPROJ" ]]; then
    info "Building Soluna.app (xcodebuild)..."
    XCODE_LOG="$BUILD_DIR/xcodebuild.log"
    if ! xcodebuild \
        -project "$XCODEPROJ" \
        -scheme SolunaReceiverMac \
        -configuration Release \
        CONFIGURATION_BUILD_DIR="$APP_OUTPUT" \
        CODE_SIGN_IDENTITY="-" \
        CODE_SIGNING_ALLOWED=YES \
        > "$XCODE_LOG" 2>&1; then
        echo ""
        warn "xcodebuild failed. Last 20 lines:"
        tail -20 "$XCODE_LOG"
        warn "Full log: $XCODE_LOG"
        warn "Soluna.app will NOT be installed. Driver-only mode."
    elif [[ ! -d "$APP_OUTPUT/SolunaReceiverMac.app" ]]; then
        warn "xcodebuild completed but SolunaReceiverMac.app not found"
        warn "Soluna.app will NOT be installed. Driver-only mode."
    else
        ok "Soluna.app built"
    fi
else
    warn "Xcode project not found at $XCODEPROJ — skipping app build"
fi

# ── Step 3: Install Soluna.driver → /Library/Audio/Plug-Ins/HAL/ ──
info "Installing Soluna.driver..."
echo "  (sudo required for /Library/Audio/Plug-Ins/HAL/)"

codesign --force --sign - --deep "$DRIVER_SRC" 2>/dev/null || true

sudo rm -rf "$DRIVER_DEST"
sudo cp -r "$DRIVER_SRC" "$DRIVER_DEST"
sudo chown -R root:wheel "$DRIVER_DEST"
sudo chmod -R 755 "$DRIVER_DEST"
ok "Soluna.driver → $DRIVER_DEST"

# ── Step 4: Install Soluna.app → /Applications/ ──────────────
if [[ -d "$APP_OUTPUT/SolunaReceiverMac.app" ]]; then
    info "Installing Soluna.app..."
    rm -rf "/Applications/Soluna.app"
    cp -r "$APP_OUTPUT/SolunaReceiverMac.app" "/Applications/Soluna.app"
    xattr -cr "/Applications/Soluna.app" 2>/dev/null || true
    ok "Soluna.app → /Applications/Soluna.app"
fi

# ── Step 5: Shared memory directory ──────────────────────────
info "Creating shared memory directory..."
if [[ ! -d "$SHM_DIR" ]]; then
    sudo mkdir -p "$SHM_DIR"
    sudo chmod 0755 "$SHM_DIR"
    sudo chown root:staff "$SHM_DIR"
    ok "$SHM_DIR created (chmod 0755, root:staff)"
else
    ok "$SHM_DIR already exists"
fi

# ── Step 6: Restart coreaudiod ───────────────────────────────
info "Restarting coreaudiod..."
sudo killall coreaudiod 2>/dev/null || true
sleep 2
ok "coreaudiod restarted"

# ── Step 7 (--headless only): solunad + LaunchAgent ──────────
if [[ "$HEADLESS" == true ]]; then
    INSTALL_DIR="$HOME/.local/bin"
    LAUNCH_AGENTS="$HOME/Library/LaunchAgents"
    SERVICE_ID="io.soluna.daemon"

    info "Installing solunad (headless mode)..."
    if [[ -f "$BUILD_DIR/solunad" ]]; then
        mkdir -p "$INSTALL_DIR"
        cp "$BUILD_DIR/solunad" "$INSTALL_DIR/solunad"
        chmod +x "$INSTALL_DIR/solunad"
        ok "solunad → $INSTALL_DIR/solunad"

        if [[ -f "$BUILD_DIR/solctl" ]]; then
            cp "$BUILD_DIR/solctl" "$INSTALL_DIR/solctl"
            chmod +x "$INSTALL_DIR/solctl"
            ok "solctl → $INSTALL_DIR/solctl"
        fi

        # Add to PATH
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

        # LaunchAgent
        info "Installing LaunchAgent..."
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
    else
        warn "solunad not found in build — skipping headless setup"
    fi
fi

# ── Verify ──────────────────────────────────────────────────
info "Verifying installation..."
sleep 2

if system_profiler SPAudioDataType 2>/dev/null | grep -q "Soluna"; then
    ok "Soluna audio device detected"
else
    warn "Soluna audio device not yet visible"
    warn "Go to System Settings > Privacy & Security > allow the driver, then:"
    warn "  sudo killall coreaudiod"
fi

if [[ -d "/Applications/Soluna.app" ]]; then
    ok "Soluna.app installed in /Applications"
fi

# ── Done ─────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}  Installation complete!${NC}"
echo ""
echo "  Next steps:"
echo "    1. System Settings → Sound → Output → Select ${BOLD}Soluna${NC}"
echo "    2. Open ${BOLD}Soluna.app${NC} from /Applications"
echo "       - Audio TX: システム音声をネットワーク配信"
echo "       - Mic TX:   マイク音声を配信"
echo "       - WAN P2P:  インターネット越しにグループ共有"
echo ""
if [[ "$HEADLESS" == true ]]; then
    echo "  Headless commands:"
    echo "    solctl status               # daemon status"
    echo "    tail -f /tmp/solunad.log    # logs"
    echo ""
fi
echo "  Uninstall:"
echo "    bash $SRC_DIR/scripts/uninstall-mac.sh"
echo ""
