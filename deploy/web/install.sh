#!/usr/bin/env bash
# Soluna install script — Mac (Apple Silicon / Intel)
# Usage: curl -fsSL https://soluna-web.fly.dev/install.sh | bash
set -euo pipefail

REPO="https://github.com/yukihamada/opensonic.git"
INSTALL_DIR="/usr/local/bin"
PLIST_DIR="$HOME/Library/LaunchAgents"
PLIST_ID="io.soluna.daemon"
# ── Color helpers ──────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; BOLD='\033[1m'; NC='\033[0m'
info()    { echo -e "${BLUE}▶${NC} $*"; }
success() { echo -e "${GREEN}✔${NC} $*"; }
warn()    { echo -e "${YELLOW}⚠${NC} $*"; }
die()     { echo -e "${RED}✖ $*${NC}" >&2; exit 1; }

echo ""
echo -e "${BOLD}  ◈ Soluna Installer${NC}"
echo -e "  Mac ネットワークオーディオ"
echo "  ──────────────────────────────────"
echo ""

# ── Platform check ─────────────────────────────────────────
[[ "$(uname)" == "Darwin" ]] || die "このスクリプトは macOS 専用です。"
ARCH=$(uname -m)
info "検出: macOS / $ARCH"

# ── Dependencies ───────────────────────────────────────────
need() {
    if ! command -v "$1" &>/dev/null; then
        echo -e "${YELLOW}  '$1' が見つかりません。${NC}"
        return 1
    fi
    return 0
}

MISSING=()
need cmake  || MISSING+=("cmake")
need git    || MISSING+=("git")
need clang  || MISSING+=("xcode-select --install (Xcode CLT)")

if (( ${#MISSING[@]} > 0 )); then
    warn "以下が必要です:"
    for m in "${MISSING[@]}"; do echo "  • $m"; done
    echo ""
    if ! need brew; then
        info "Homebrew をインストール中..."
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
        eval "$(/opt/homebrew/bin/brew shellenv)" 2>/dev/null || true
    fi
    if [[ " ${MISSING[*]} " == *"cmake"* ]]; then
        info "cmake をインストール中..."
        brew install cmake
    fi
    if [[ " ${MISSING[*]} " == *"git"* ]]; then
        info "git をインストール中..."
        brew install git
    fi
    if ! xcode-select -p &>/dev/null; then
        info "Xcode Command Line Tools をインストール中..."
        xcode-select --install
        echo "  インストールが完了したら Enter を押してください..."
        read -r
    fi
fi

# ── Clone & Build ──────────────────────────────────────────
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

info "ソースをクローン中..."
git clone --depth=1 "$REPO" "$TMP_DIR/opensonic" 2>&1 | grep -E "Cloning|done\."

NPROC="$(sysctl -n hw.logicalcpu)"
SRC_DIR="$TMP_DIR/opensonic"
BUILD_DIR="$SRC_DIR/build"

info "solunad をビルド中 (1〜2分)..."
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSOLUNA_BUILD_TESTS=OFF \
    -Wno-dev -DCMAKE_WARN_DEPRECATED=OFF \
    > "$TMP_DIR/cmake.log" 2>&1 || { cat "$TMP_DIR/cmake.log"; die "cmake configure 失敗"; }

cmake --build "$BUILD_DIR" --target solunad --parallel "$NPROC" \
    > "$TMP_DIR/build.log" 2>&1 || { tail -30 "$TMP_DIR/build.log"; die "solunad ビルド失敗"; }

BINARY="$BUILD_DIR/solunad"
[[ -f "$BINARY" ]] || die "バイナリが見つかりません: $BINARY"
success "solunad ビルド完了"

# ── Install solunad binary ────────────────────────────────
info "solunad をインストール中..."
if [[ -w "$INSTALL_DIR" ]]; then
    cp "$BINARY" "$INSTALL_DIR/solunad"
else
    sudo cp "$BINARY" "$INSTALL_DIR/solunad"
fi
chmod +x "$INSTALL_DIR/solunad"
success "solunad → $INSTALL_DIR/solunad"

# ── Shared memory directory ───────────────────────────────
SHM_DIR="/private/var/db/soluna"
if [[ ! -d "$SHM_DIR" ]]; then
    sudo mkdir -p "$SHM_DIR"
    sudo chmod 777 "$SHM_DIR"
    success "共有メモリディレクトリ作成: $SHM_DIR"
fi

# ── Install launchd service ────────────────────────────────
PLIST_SRC="$SRC_DIR/apps/daemon/io.soluna.daemon.plist"
PLIST_DST="$PLIST_DIR/$PLIST_ID.plist"

if [[ -f "$PLIST_SRC" ]]; then
    mkdir -p "$PLIST_DIR"
    cp "$PLIST_SRC" "$PLIST_DST"

    # Unload if already running
    launchctl bootout "gui/$UID/$PLIST_ID" 2>/dev/null || true

    launchctl bootstrap "gui/$UID" "$PLIST_DST"
    success "launchd サービスを登録 (自動起動 ON)"
else
    warn "plist が見つかりません。手動で launchd 登録してください。"
fi

# ── Set system output ─────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}✔ インストール完了！${NC}"
echo ""
echo "  ┌──────────────────────────────────────────────────┐"
echo "  │                                                  │"
echo "  │  次のステップ:                                   │"
echo "  │                                                  │"
echo "  │  1. システム設定 → サウンド → 出力               │"
echo "  │     → 「Soluna」を選択                           │"
echo "  │                                                  │"
echo "  │  2. ブラウザで http://localhost:8400 を開く       │"
echo "  │     → ダッシュボードが表示されます               │"
echo "  │                                                  │"
echo "  │  3. iPhone / ブラウザ で受信開始                  │"
echo "  │                                                  │"
echo "  └──────────────────────────────────────────────────┘"
echo ""
echo "  コマンド:"
echo "    停止:          launchctl stop  $PLIST_ID"
echo "    起動:          launchctl start $PLIST_ID"
echo "    ログ:          tail -f /tmp/solunad.log"
echo "    アンインストール: launchctl bootout gui/\$UID/$PLIST_ID"
echo "                    sudo rm $INSTALL_DIR/solunad"
echo ""
