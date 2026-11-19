#!/usr/bin/env bash
# Configure + build the macOS Apple clang ASan+UBSan build used by fuzz_mac.py.
#
#   ./BuildMac.sh -d -x            # once: build the deps (deps/build/<arch>/BambuStudio_deps)
#   fuzz/build_asan_mac.sh         # -> build-asan/src/BambuStudio.app (+ print_bot-mcp)
#
# Env overrides: BUILD_DIR (default <repo>/build-asan), ARCH (default: uname -m),
# DEPS_PREFIX (default <repo>/deps/build/<arch>/BambuStudio_deps/usr/local), OSX_DEPLOYMENT_TARGET (default 11.3),
# JOBS (default: all cores). No sudo needed; Xcode (or the Command Line Tools) must be installed and its license accepted.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="${ARCH:-$(uname -m)}"
BUILD_DIR="${BUILD_DIR:-$REPO/build-asan}"
DEPS_PREFIX="${DEPS_PREFIX:-$REPO/deps/build/$ARCH/BambuStudio_deps/usr/local}"
OSX_DEPLOYMENT_TARGET="${OSX_DEPLOYMENT_TARGET:-11.3}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
[ -d "$DEPS_PREFIX" ] || { echo "deps prefix $DEPS_PREFIX not found: run ./BuildMac.sh -d -x first"; exit 1; }
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    cmake -S "$REPO" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DBBL_RELEASE_TO_PUBLIC=1 -DBBL_INTERNAL_TESTING=0 \
        -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" -DCMAKE_MACOSX_RPATH=ON -DCMAKE_INSTALL_RPATH="$DEPS_PREFIX" \
        -DCMAKE_MACOSX_BUNDLE=ON -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET="$OSX_DEPLOYMENT_TARGET" \
        -DCMAKE_C_FLAGS="$SAN" -DCMAKE_CXX_FLAGS="$SAN" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined" \
        -DCMAKE_MODULE_LINKER_FLAGS="-fsanitize=address,undefined"
fi
cmake --build "$BUILD_DIR" -j "$JOBS"
ls -d "$BUILD_DIR"/src/*.app
