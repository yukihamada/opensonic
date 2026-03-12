#!/usr/bin/env bash
# Soluna RX installer — Raspberry Pi / Linux
# Usage: curl -fsSL https://soluna-web.fly.dev/install-rx.sh | sudo bash
set -euo pipefail

REPO="https://github.com/yukihamada/opensonic.git"
SERVICE_ID="soluna"
INSTALL_BIN="/usr/local/bin/soluna"

# ── Color helpers ──────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; BOLD='\033[1m'; NC='\033[0m'
info()    { echo -e "${BLUE}▶${NC} $*"; }
success() { echo -e "${GREEN}✔${NC} $*"; }
warn()    { echo -e "${YELLOW}⚠${NC} $*"; }
die()     { echo -e "${RED}✖ $*${NC}" >&2; exit 1; }

echo ""
echo -e "${BOLD}  ◈ Soluna Installer${NC}"
echo -e "  Raspberry Pi / Linux 統合 CLI（受信・DJ・リレー）"
echo "  ──────────────────────────────────────────────"
echo ""

# ── Platform check ─────────────────────────────────────────
[[ "$(uname)" == "Linux" ]] || die "このスクリプトは Linux 専用です。macOS は install.sh を使用してください。"
[[ $EUID -eq 0 ]] || die "root 権限が必要です。sudo で実行してください。"

ARCH=$(uname -m)
info "検出: Linux / $ARCH"

# ── Dependencies ───────────────────────────────────────────
info "依存パッケージをインストール中..."
if command -v apt-get &>/dev/null; then
    apt-get update -qq
    apt-get install -y -qq cmake g++ git libasound2-dev > /dev/null 2>&1
elif command -v dnf &>/dev/null; then
    dnf install -y -q cmake gcc-c++ git alsa-lib-devel > /dev/null 2>&1
elif command -v pacman &>/dev/null; then
    pacman -Sy --noconfirm cmake gcc git alsa-lib > /dev/null 2>&1
else
    warn "パッケージマネージャーを検出できません。cmake, g++, git, libasound2-dev が必要です。"
fi
success "依存パッケージ OK"

# ── Clone & Build ──────────────────────────────────────────
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

info "ソースをクローン中..."
git clone --depth=1 "$REPO" "$TMP_DIR/opensonic" 2>&1 | grep -E "Cloning|done\." || true

NPROC="$(nproc 2>/dev/null || echo 2)"
SRC_DIR="$TMP_DIR/opensonic"
BUILD_DIR="$SRC_DIR/build"

info "soluna をビルド中..."
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -Wno-dev -DCMAKE_WARN_DEPRECATED=OFF \
    > "$TMP_DIR/cmake.log" 2>&1 || { cat "$TMP_DIR/cmake.log"; die "cmake configure 失敗"; }

cmake --build "$BUILD_DIR" --target soluna --parallel "$NPROC" \
    > "$TMP_DIR/build.log" 2>&1 || { tail -30 "$TMP_DIR/build.log"; die "ビルド失敗"; }

BINARY="$BUILD_DIR/soluna"
[[ -f "$BINARY" ]] || die "バイナリが見つかりません: $BINARY"
success "soluna ビルド完了"

# ── Install binary ────────────────────────────────────────
cp "$BINARY" "$INSTALL_BIN"
chmod +x "$INSTALL_BIN"
setcap cap_net_raw,cap_net_bind_service,cap_sys_nice=ep "$INSTALL_BIN" 2>/dev/null || true
success "soluna → $INSTALL_BIN"

# ── Create soluna user ────────────────────────────────────
if ! id -u soluna &>/dev/null; then
    useradd --system --no-create-home --shell /usr/sbin/nologin --groups audio soluna 2>/dev/null || true
    success "soluna ユーザーを作成"
fi

# ── Install systemd service ───────────────────────────────
cat > /etc/systemd/system/$SERVICE_ID.service << 'UNIT'
[Unit]
Description=Soluna RX — Network Audio Receiver
After=network-online.target sound.target
Wants=network-online.target

[Service]
Type=simple
User=soluna
Environment=SOLUNA_DEVICE=default
Environment=SOLUNA_GROUP=239.69.0.1
Environment=SOLUNA_PORT=5004
Environment=SOLUNA_CHANNELS=2
ExecStart=/usr/local/bin/soluna \
    --output alsa \
    --device ${SOLUNA_DEVICE} \
    --group ${SOLUNA_GROUP} \
    --port ${SOLUNA_PORT} \
    --channels ${SOLUNA_CHANNELS}
Restart=always
RestartSec=3
Nice=-15
LimitRTPRIO=99
LimitMEMLOCK=infinity

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable "$SERVICE_ID"
systemctl restart "$SERVICE_ID"
success "systemd サービスを登録 ($SERVICE_ID)"

# ── Done ──────────────────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}✔ インストール完了！${NC}"
echo ""
echo "  soluna はバックグラウンドで起動しました。"
echo "  同じネットワーク上の Mac から Soluna で音声を送信してください。"
echo ""
echo "  設定:"
echo "    マルチキャスト: 239.69.0.1:5004 (デフォルト)"
echo "    ALSA デバイス:  default"
echo ""
echo "  コマンド:"
echo "    ステータス: systemctl status $SERVICE_ID"
echo "    ログ:       journalctl -fu $SERVICE_ID"
echo "    停止:       systemctl stop $SERVICE_ID"
echo "    無効化:     systemctl disable $SERVICE_ID"
echo ""
echo "  ALSA デバイスを変更する場合:"
echo "    sudo systemctl edit $SERVICE_ID"
echo "    → Environment=SOLUNA_DEVICE=hw:1,0"
echo ""
