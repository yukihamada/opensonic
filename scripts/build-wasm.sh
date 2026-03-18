#!/bin/bash
# Build Soluna WASM player
# Usage: bash scripts/build-wasm.sh
set -euo pipefail

cd "$(dirname "$0")/.."

echo "Building soluna-wasm..."
cd crates/soluna-wasm
wasm-pack build --target web --release --out-dir ../../web/wasm/pkg

echo "Copying to deploy..."
mkdir -p ../../deploy/web/wasm
cp -r ../../web/wasm/* ../../deploy/web/wasm/

echo "Done! WASM size:"
ls -lh ../../web/wasm/pkg/*.wasm
