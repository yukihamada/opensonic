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
SIGN=true
APP_SIGN_ID="Developer ID Application: Yuki Hamada (5BV85JW8US)"
PKG_SIGN_ID="Developer ID Installer: Yuki Hamada (5BV85JW8US)"
TEAM_ID="5BV85JW8US"

for arg in "$@"; do
    case "$arg" in
        --universal) UNIVERSAL=true ;;
        --no-sign)   SIGN=false ;;
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
mkdir -p "$STAGING/payload/Applications"
mkdir -p "$STAGING/payload/usr/local/bin"

# ── Step 1: Build with cmake (arm64) ──────────────────────
info "Building Soluna core (arm64)..."
cmake -B "$BUILD_DIR/arm64" -S "$SRC_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="13.0" \
    > "$BUILD_DIR/cmake-arm64.log" 2>&1

cmake --build "$BUILD_DIR/arm64" -j"$NPROC" >> "$BUILD_DIR/cmake-arm64.log" 2>&1
ok "arm64 build complete"

if [ "$UNIVERSAL" = true ]; then
    info "Building Soluna core (x86_64)..."
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
# Clean previous build artifacts (may be root-owned from signing)
if [[ -d "$APP_OUTPUT" ]] && ! rm -rf "$APP_OUTPUT" 2>/dev/null; then
    # Root-owned artifacts from previous signed build — use timestamped dir
    APP_OUTPUT="$BUILD_DIR/app-output-$(date +%s)"
fi
mkdir -p "$APP_OUTPUT"

if [[ -d "$XCODEPROJ" ]]; then
    info "Building Soluna.app..."
    if [ "$SIGN" = true ]; then
        xcodebuild \
            -project "$XCODEPROJ" \
            -scheme SolunaReceiverMac \
            -configuration Release \
            CONFIGURATION_BUILD_DIR="$APP_OUTPUT" \
            CODE_SIGN_IDENTITY="$APP_SIGN_ID" \
            DEVELOPMENT_TEAM="$TEAM_ID" \
            CODE_SIGN_STYLE=Manual \
            > "$BUILD_DIR/xcodebuild.log" 2>&1
    else
        xcodebuild \
            -project "$XCODEPROJ" \
            -scheme SolunaReceiverMac \
            -configuration Release \
            CONFIGURATION_BUILD_DIR="$APP_OUTPUT" \
            CODE_SIGN_IDENTITY="-" \
            CODE_SIGNING_ALLOWED=YES \
            > "$BUILD_DIR/xcodebuild.log" 2>&1
    fi
    ok "Soluna.app built"
else
    fail "Xcode project not found: $XCODEPROJ"
fi

# ── Step 3: Stage payload ──────────────────────────────────
info "Staging payload..."

# App
if [[ -d "$APP_OUTPUT/SolunaReceiverMac.app" ]]; then
    cp -r "$APP_OUTPUT/SolunaReceiverMac.app" "$STAGING/payload/Applications/Soluna.app"
    ok "Soluna.app staged"
fi

# CLI tools
if [ "$UNIVERSAL" = true ] && [[ -f "$BUILD_DIR/x64/solunad" ]] && [[ -f "$BUILD_DIR/arm64/solunad" ]]; then
    lipo -create \
        "$BUILD_DIR/arm64/solunad" \
        "$BUILD_DIR/x64/solunad" \
        -output "$STAGING/payload/usr/local/bin/solunad"
    ok "solunad (universal) staged"
elif [[ -f "$BUILD_DIR/arm64/solunad" ]]; then
    cp "$BUILD_DIR/arm64/solunad" "$STAGING/payload/usr/local/bin/solunad"
    ok "solunad (arm64) staged"
else
    echo "  ! solunad not found — skipping"
fi
chmod +x "$STAGING/payload/usr/local/bin/"* 2>/dev/null || true

# ── Step 4: Sign the app and solunad binary ────────────────
if [ "$SIGN" = true ]; then
    info "Signing binaries..."
    codesign --force --options runtime --sign "$APP_SIGN_ID" \
        "$STAGING/payload/usr/local/bin/solunad" 2>/dev/null && ok "solunad signed" || echo "  ! solunad signing skipped"
    codesign --force --deep --options runtime --sign "$APP_SIGN_ID" \
        "$STAGING/payload/Applications/Soluna.app" 2>/dev/null && ok "Soluna.app signed" || echo "  ! Soluna.app signing skipped"
fi

# ── Step 5: pkgbuild ───────────────────────────────────────
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

# ── Step 6: productbuild (+ sign) ─────────────────────────
info "Building Soluna-mac.pkg..."
if [ "$SIGN" = true ]; then
    productbuild \
        --distribution "$SCRIPT_DIR/pkg-resources/Distribution.xml" \
        --resources "$SCRIPT_DIR/pkg-resources" \
        --package-path "$BUILD_DIR" \
        --sign "$PKG_SIGN_ID" \
        "$SRC_DIR/Soluna-mac.pkg" \
        > "$BUILD_DIR/productbuild.log" 2>&1
    ok "Soluna-mac.pkg created (signed)"
else
    productbuild \
        --distribution "$SCRIPT_DIR/pkg-resources/Distribution.xml" \
        --resources "$SCRIPT_DIR/pkg-resources" \
        --package-path "$BUILD_DIR" \
        "$SRC_DIR/Soluna-mac.pkg" \
        > "$BUILD_DIR/productbuild.log" 2>&1
    ok "Soluna-mac.pkg created (unsigned)"
fi

# ── Step 7: Notarize ──────────────────────────────────────
if [ "$SIGN" = true ]; then
    info "Notarizing Soluna-mac.pkg..."
    if xcrun notarytool submit "$SRC_DIR/Soluna-mac.pkg" \
        --keychain-profile "notarytool-profile" \
        --wait > "$BUILD_DIR/notarize.log" 2>&1; then
        ok "Notarization succeeded"
        xcrun stapler staple "$SRC_DIR/Soluna-mac.pkg" >> "$BUILD_DIR/notarize.log" 2>&1
        ok "Stapled notarization ticket"
    else
        echo -e "  ${RED}! Notarization failed — check build-pkg/notarize.log${NC}"
        echo -e "  ${BLUE}Tip: Run: xcrun notarytool store-credentials notarytool-profile --apple-id mail@yukihamada.jp --team-id $TEAM_ID${NC}"
    fi
fi

# ── Done ───────────────────────────────────────────────────
echo ""
PKG_SIZE=$(du -h "$SRC_DIR/Soluna-mac.pkg" | cut -f1)
echo -e "${GREEN}${BOLD}  ◈ Soluna-mac.pkg ($PKG_SIZE) → $SRC_DIR/Soluna-mac.pkg${NC}"
echo ""
