#!/usr/bin/env bash
# Configure + build the clang ASan+UBSan GUI build used by the Linux fuzzer.
#
#   fuzz/build_asan.sh            # configure (first time) and build
#   BUILD_DIR=/x/asan fuzz/build_asan.sh
#
# Env overrides (all default to repo-relative paths):
#   BUILD_DIR    ASan build dir                     (default <repo>/build-asan)
#   DEPS_PREFIX  the normal deps install prefix     (default <repo>/deps/build/destdir/usr/local)
#   TBB_PREFIX   clang-built TBB install prefix     (default <repo>/build-asan-deps/prefix)
#   TBB_SRC      TBB sources (from the deps build)  (default <repo>/deps/build/dep_TBB-prefix/src/dep_TBB)
#   JOBS         parallel jobs                      (default: nproc)
#   CC/CXX       compilers                          (default clang/clang++)
#
# Why a separate TBB: the deps build compiles TBB with GCC + LTO; GCC LTO objects can't be linked by clang/lld,
# so TBB is rebuilt once with clang (static, no LTO) into TBB_PREFIX and found first via CMAKE_PREFIX_PATH.
# Precompiled headers (SLIC3R_PCH=ON) are the tested configuration.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO/build-asan}"
DEPS_PREFIX="${DEPS_PREFIX:-$REPO/deps/build/destdir/usr/local}"
TBB_PREFIX="${TBB_PREFIX:-$REPO/build-asan-deps/prefix}"
TBB_SRC="${TBB_SRC:-$REPO/deps/build/dep_TBB-prefix/src/dep_TBB}"
JOBS="${JOBS:-$(nproc)}"
CC="${CC:-clang}"; CXX="${CXX:-clang++}"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"

[ -d "$DEPS_PREFIX" ] || { echo "deps prefix $DEPS_PREFIX not found: build deps first (see the main build docs)"; exit 1; }

if [ ! -f "$TBB_PREFIX/lib/cmake/TBB/TBBConfig.cmake" ]; then
    [ -d "$TBB_SRC" ] || { echo "TBB sources not found at $TBB_SRC (set TBB_SRC)"; exit 1; }
    echo "== building TBB with $CXX into $TBB_PREFIX"
    cmake -S "$TBB_SRC" -B "$TBB_PREFIX/../tbb-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DBUILD_SHARED_LIBS=OFF -DTBB_TEST=OFF -DTBB_STRICT=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$TBB_PREFIX"
    cmake --build "$TBB_PREFIX/../tbb-build" -j "$JOBS" --target install
fi

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    echo "== configuring $BUILD_DIR"
    cmake -S "$REPO" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_C_FLAGS="$SAN" -DCMAKE_CXX_FLAGS="$SAN" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined -fuse-ld=lld" \
        -DCMAKE_MODULE_LINKER_FLAGS="-fsanitize=address,undefined -fuse-ld=lld" \
        -DCMAKE_PREFIX_PATH="$TBB_PREFIX;$DEPS_PREFIX" -DTBB_DIR="$TBB_PREFIX/lib/cmake/TBB" \
        -DSLIC3R_STATIC=1 -DSLIC3R_GTK=3 -DSLIC3R_PCH=ON -DSLIC3R_ASAN=OFF -DBBL_RELEASE_TO_PUBLIC=1
fi
echo "== building $BUILD_DIR"
cmake --build "$BUILD_DIR" -j "$JOBS"
echo "ASan GUI binary: $BUILD_DIR/src/bambu-studio"
