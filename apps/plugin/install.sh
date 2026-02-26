#!/usr/bin/env bash
# install.sh — Build, sign, and install Soluna.driver
# Usage: bash apps/plugin/install.sh [build_dir]
set -euo pipefail

BUILD_DIR="${1:-build}"
DRIVER="${BUILD_DIR}/apps/plugin/Soluna.driver"

if [[ ! -d "${DRIVER}" ]]; then
    echo "Error: ${DRIVER} not found. Build first:"
    echo "  cmake --build ${BUILD_DIR} --target Soluna"
    exit 1
fi

echo "==> Signing ${DRIVER} (ad-hoc)"
codesign --force --sign - --deep "${DRIVER}"

DEST="/Library/Audio/Plug-Ins/HAL/Soluna.driver"

echo "==> Installing to ${DEST}"
sudo rm -rf "${DEST}"
sudo cp -r "${DRIVER}" "${DEST}"
sudo chown -R root:wheel "${DEST}"
sudo chmod -R 755 "${DEST}"

echo "==> Restarting coreaudiod"
sudo killall coreaudiod 2>/dev/null || true

echo ""
echo "Done! Open System Settings > Sound > Output and select 'Soluna'."
echo "Then run: solunad --tx --device soluna --channels 2"
