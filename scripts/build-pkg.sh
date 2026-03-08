#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────
# build-pkg.sh — Soluna macOS .pkg インストーラをビルド
#
# Usage:
#   bash scripts/build-pkg.sh           # arm64 only
#   bash scripts/build-pkg.sh --universal  # arm64 + x86_64
#
# Output: Soluna-mac.pkg
# ──────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SRC_DIR/build-pkg"
STAGING="$BUILD_DIR/staging"
PKG_ID="io.soluna.pkg"
VER="${SOLUNA_VERSION:-0.2.0}"
NPROC=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
UNIVERSAL=false

for arg in "$@"; do
    case "$arg" in
        --universal) UNIVERSAL=true ;;
    esac
done

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

info()  { echo -e "${BLUE}==>${NC} ${BOLD}$*${NC}"; }
ok()    { echo -e "${GREEN}  ✓${NC} $*"; }
fail()  { echo -e "${RED}  ✗ $*${NC}"; exit 1; }

echo ""
echo -e "${BOLD}  ◈ Soluna .pkg Builder${NC}"
echo ""

# ── Pre-flight ─────────────────────────────────────────────
[[ "$(uname)" == "Darwin" ]] || fail "macOS only"
command -v cmake      >/dev/null 2>&1 || fail "cmake not found"
command -v xcodebuild >/dev/null 2>&1 || fail "xcodebuild not found"
command -v pkgbuild   >/dev/null 2>&1 || fail "pkgbuild not found"
command -v productbuild >/dev/null 2>&1 || fail "productbuild not found"

# ── Clean staging ──────────────────────────────────────────
rm -rf "$STAGING"
mkdir -p "$STAGING/payload/Library/Audio/Plug-Ins/HAL"
mkdir -p "$STAGING/payload/Applications"
mkdir -p "$STAGING/payload/usr/local/bin"

# ── Step 1: Build with cmake (arm64) ──────────────────────
info "Building Soluna core + driver (arm64)..."
cmake -B "$BUILD_DIR/arm64" -S "$SRC_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="13.0" \
    > "$BUILD_DIR/cmake-arm64.log" 2>&1

cmake --build "$BUILD_DIR/arm64" -j"$NPROC" >> "$BUILD_DIR/cmake-arm64.log" 2>&1
ok "arm64 build complete"

if [ "$UNIVERSAL" = true ]; then
    info "Building Soluna core + driver (x86_64)..."
    cmake -B "$BUILD_DIR/x64" -S "$SRC_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="x86_64" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="13.0" \
        > "$BUILD_DIR/cmake-x64.log" 2>&1

    cmake --build "$BUILD_DIR/x64" -j"$NPROC" >> "$BUILD_DIR/cmake-x64.log" 2>&1
    ok "x86_64 build complete"
fi

# ── Step 2: Build Soluna.app (xcodebuild) ──────────────────
XCODEPROJ="$SRC_DIR/apps/mac-rx/SolunaReceiverMac.xcodeproj"
APP_OUTPUT="$BUILD_DIR/app-output"

if [[ -d "$XCODEPROJ" ]]; then
    info "Building Soluna.app..."
    xcodebuild \
        -project "$XCODEPROJ" \
        -scheme SolunaReceiverMac \
        -configuration Release \
        CONFIGURATION_BUILD_DIR="$APP_OUTPUT" \
        CODE_SIGN_IDENTITY="-" \
        CODE_SIGNING_ALLOWED=YES \
        > "$BUILD_DIR/xcodebuild.log" 2>&1
    ok "Soluna.app built"
else
    fail "Xcode project not found: $XCODEPROJ"
fi

# ── Step 3: Stage payload ──────────────────────────────────
info "Staging payload..."

# Driver
DRIVER_SRC="$BUILD_DIR/arm64/apps/plugin/Soluna.driver"
[[ -d "$DRIVER_SRC" ]] || fail "Soluna.driver not found in build"

if [ "$UNIVERSAL" = true ]; then
    # Create universal driver binary
    DRIVER_BIN="Soluna.driver/Contents/MacOS/Soluna"
    cp -r "$DRIVER_SRC" "$STAGING/payload/Library/Audio/Plug-Ins/HAL/Soluna.driver"
    if [[ -f "$BUILD_DIR/x64/apps/plugin/$DRIVER_BIN" ]] && [[ -f "$BUILD_DIR/arm64/apps/plugin/$DRIVER_BIN" ]]; then
        lipo -create \
            "$BUILD_DIR/arm64/apps/plugin/$DRIVER_BIN" \
            "$BUILD_DIR/x64/apps/plugin/$DRIVER_BIN" \
            -output "$STAGING/payload/Library/Audio/Plug-Ins/HAL/$DRIVER_BIN"
        ok "Universal driver binary created"
    fi
else
    cp -r "$DRIVER_SRC" "$STAGING/payload/Library/Audio/Plug-Ins/HAL/Soluna.driver"
fi
ok "Soluna.driver staged"

# App
if [[ -d "$APP_OUTPUT/SolunaReceiverMac.app" ]]; then
    cp -r "$APP_OUTPUT/SolunaReceiverMac.app" "$STAGING/payload/Applications/Soluna.app"
    ok "Soluna.app staged"
fi

# CLI tools
for tool in solunad solctl; do
    if [ "$UNIVERSAL" = true ] && [[ -f "$BUILD_DIR/x64/$tool" ]] && [[ -f "$BUILD_DIR/arm64/$tool" ]]; then
        lipo -create \
            "$BUILD_DIR/arm64/$tool" \
            "$BUILD_DIR/x64/$tool" \
            -output "$STAGING/payload/usr/local/bin/$tool"
        ok "$tool (universal) staged"
    elif [[ -f "$BUILD_DIR/arm64/$tool" ]]; then
        cp "$BUILD_DIR/arm64/$tool" "$STAGING/payload/usr/local/bin/$tool"
        ok "$tool (arm64) staged"
    else
        echo "  ! $tool not found — skipping"
    fi
done
chmod +x "$STAGING/payload/usr/local/bin/"* 2>/dev/null || true

# ── Step 4: pkgbuild ───────────────────────────────────────
info "Building component.pkg..."
pkgbuild \
    --root "$STAGING/payload" \
    --scripts "$SCRIPT_DIR/pkg-scripts" \
    --identifier "$PKG_ID" \
    --version "$VER" \
    --install-location "/" \
    "$BUILD_DIR/component.pkg" \
    > "$BUILD_DIR/pkgbuild.log" 2>&1
ok "component.pkg created"

# ── Step 5: productbuild ───────────────────────────────────
info "Building Soluna-mac.pkg..."
productbuild \
    --distribution "$SCRIPT_DIR/pkg-resources/Distribution.xml" \
    --resources "$SCRIPT_DIR/pkg-resources" \
    --package-path "$BUILD_DIR" \
    "$SRC_DIR/Soluna-mac.pkg" \
    > "$BUILD_DIR/productbuild.log" 2>&1
ok "Soluna-mac.pkg created"

# ── Done ───────────────────────────────────────────────────
echo ""
PKG_SIZE=$(du -h "$SRC_DIR/Soluna-mac.pkg" | cut -f1)
echo -e "${GREEN}${BOLD}  ◈ Soluna-mac.pkg ($PKG_SIZE) → $SRC_DIR/Soluna-mac.pkg${NC}"
echo ""
