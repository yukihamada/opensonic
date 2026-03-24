#!/usr/bin/env bash
#
# Build libopus as a standalone WASM module for browser use.
#
# Prerequisites:
#   - Emscripten SDK (emsdk) installed and activated
#     https://emscripten.org/docs/getting_started/downloads.html
#     source emsdk/emsdk_env.sh
#
# Output:
#   web/wasm/opus-wasm/opus-decoder.wasm  (~120KB gzipped, ~300KB raw)
#
# Usage:
#   cd /path/to/opensonic
#   bash web/wasm/build-opus-wasm.sh
#
# The resulting .wasm is a standalone WebAssembly module (not Emscripten JS glue)
# that exports:
#   opus_decoder_get_size(channels) -> int
#   opus_decoder_init(decoder_ptr, sample_rate, channels) -> int
#   opus_decode_float(decoder_ptr, data_ptr, data_len, pcm_ptr, frame_size, decode_fec) -> int
#   malloc(size) -> ptr
#   free(ptr)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-opus-wasm"
OUTPUT_DIR="$SCRIPT_DIR/opus-wasm"
OPUS_VERSION="${OPUS_VERSION:-1.5.2}"
OPUS_URL="https://downloads.xiph.org/releases/opus/opus-${OPUS_VERSION}.tar.gz"

# Check for emcc
if ! command -v emcc &>/dev/null; then
    echo "ERROR: emcc not found. Install and activate Emscripten SDK first:"
    echo "  git clone https://github.com/emscripten-core/emsdk.git"
    echo "  cd emsdk && ./emsdk install latest && ./emsdk activate latest"
    echo "  source emsdk_env.sh"
    exit 1
fi

echo "=== Building libopus ${OPUS_VERSION} for WASM ==="

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Download opus source if not cached
OPUS_TAR="opus-${OPUS_VERSION}.tar.gz"
if [ ! -f "$OPUS_TAR" ]; then
    echo "Downloading opus ${OPUS_VERSION}..."
    curl -L -o "$OPUS_TAR" "$OPUS_URL"
fi

# Extract
OPUS_SRC="opus-${OPUS_VERSION}"
if [ ! -d "$OPUS_SRC" ]; then
    echo "Extracting..."
    tar xzf "$OPUS_TAR"
fi

cd "$OPUS_SRC"

# Configure with Emscripten (disable everything except float decoder)
echo "Configuring..."
emconfigure ./configure \
    --disable-shared \
    --enable-static \
    --disable-doc \
    --disable-extra-programs \
    --disable-asm \
    --disable-rtcd \
    --disable-intrinsics \
    --disable-hardening \
    --host=wasm32-unknown-emscripten \
    CFLAGS="-O2 -fno-exceptions -DOPUS_BUILD" \
    --prefix="$BUILD_DIR/install"

# Build
echo "Building..."
emmake make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) clean
emmake make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
emmake make install

echo "Linking standalone WASM..."

# Create a minimal C wrapper that exposes only the decoder functions we need.
# This avoids exporting internal symbols and keeps the WASM small.
cat > "$BUILD_DIR/opus_decoder_wrapper.c" << 'WRAPPER_EOF'
#include <opus/opus.h>
#include <stdlib.h>

// These are the only functions we need for decoding.
// They are already in libopus; we re-export them explicitly.

// We also export malloc/free for WASM heap management.

// Ensure the linker includes these symbols:
int wrapper_opus_decoder_get_size(int channels) {
    return opus_decoder_get_size(channels);
}

int wrapper_opus_decoder_init(OpusDecoder *st, int Fs, int channels) {
    return opus_decoder_init(st, Fs, channels);
}

int wrapper_opus_decode_float(OpusDecoder *st, const unsigned char *data,
                               int len, float *pcm, int frame_size,
                               int decode_fec) {
    return opus_decode_float(st, data, len, pcm, frame_size, decode_fec);
}
WRAPPER_EOF

# Compile the wrapper + link with libopus → standalone .wasm
mkdir -p "$OUTPUT_DIR"

emcc \
    -O2 \
    -s STANDALONE_WASM=1 \
    -s EXPORTED_FUNCTIONS='["_opus_decoder_get_size","_opus_decoder_init","_opus_decode_float","_malloc","_free"]' \
    -s EXPORTED_RUNTIME_METHODS='[]' \
    -s INITIAL_MEMORY=16777216 \
    -s MAXIMUM_MEMORY=134217728 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s TOTAL_STACK=65536 \
    -s ERROR_ON_UNDEFINED_SYMBOLS=0 \
    --no-entry \
    -I"$BUILD_DIR/install/include" \
    -L"$BUILD_DIR/install/lib" \
    "$BUILD_DIR/opus_decoder_wrapper.c" \
    -lopus \
    -o "$OUTPUT_DIR/opus-decoder.wasm"

# Report size
WASM_SIZE=$(wc -c < "$OUTPUT_DIR/opus-decoder.wasm" | tr -d ' ')
WASM_SIZE_KB=$((WASM_SIZE / 1024))
echo ""
echo "=== Build complete ==="
echo "Output: $OUTPUT_DIR/opus-decoder.wasm (${WASM_SIZE_KB} KB)"
echo ""
echo "To serve: ensure your web server serves /wasm/opus-wasm/opus-decoder.wasm"
echo "          with Content-Type: application/wasm"
echo ""
echo "Browsers without WebCodecs will now use this WASM decoder automatically."

# Cleanup (keep the build dir for incremental rebuilds)
echo "Build artifacts in: $BUILD_DIR (safe to delete)"
