#!/usr/bin/env bash
# Builds the DAVE end-to-end encryption stack (OpenSSL 3 for Switch, Cisco
# mlspp, Discord libdave) into a prefix.
#
#   scripts/build-deps.sh switch [prefix]   # inside devkitpro/devkita64
#   scripts/build-deps.sh linux  [prefix]   # host build for the test CLI
set -euo pipefail

PLATFORM=${1:?usage: build-deps.sh switch|linux [prefix]}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${2:-$ROOT/deps/$PLATFORM}
SRC=$ROOT/deps/src
BUILD=$ROOT/deps/build-$PLATFORM
JOBS=$(nproc)

OPENSSL_TAG=openssl-3.0.15
MLSPP_COMMIT=1cc50a124a3bc4e143a787ec934280dc70c1034d  # the version libdave pins
JSON_TAG=v3.11.3
LIBDAVE_COMMIT=6396c90aedbb9a6ee117ea9ce1ce9832af6a3375

mkdir -p "$SRC" "$BUILD" "$PREFIX"

fetch() {  # fetch <dir> <url> <ref>
  if [ ! -d "$SRC/$1/.git" ]; then
    git clone -q "$2" "$SRC/$1"
  fi
  git -C "$SRC/$1" fetch -q --tags origin "$3" 2>/dev/null || true
  git -C "$SRC/$1" checkout -q "$3"
}

fetch json https://github.com/nlohmann/json "$JSON_TAG"
fetch mlspp https://github.com/cisco/mlspp "$MLSPP_COMMIT"
fetch libdave https://github.com/discord/libdave "$LIBDAVE_COMMIT"

CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_INSTALL_PREFIX="$PREFIX"
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON)

if [ "$PLATFORM" = switch ]; then
  : "${DEVKITPRO:=/opt/devkitpro}"
  CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake")
  fetch openssl https://github.com/openssl/openssl "$OPENSSL_TAG"
  if [ ! -f "$PREFIX/lib/libcrypto.a" ]; then
    rm -rf "$BUILD/openssl" && cp -r "$SRC/openssl" "$BUILD/openssl"
    (
      cd "$BUILD/openssl"
      export PATH="$DEVKITPRO/devkitA64/bin:$PATH"
      export CC=aarch64-none-elf-gcc AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib
      export CFLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections -fdata-sections -O2 -D__SWITCH__ -I$DEVKITPRO/libnx/include -DOPENSSL_NO_SECURE_MEMORY -DNO_SYSLOG"
      # No OS services we can't provide: entropy comes from getentropy(),
      # which platform/switch/compat implements with the kernel CSPRNG.
      ./Configure linux-generic64 no-asm no-shared no-dso no-threads no-sock no-ui-console no-tests \
        no-engine no-afalgeng no-async no-apps --prefix="$PREFIX" --openssldir="$PREFIX/ssl" \
        -DOPENSSL_RAND_SEED_OS >/dev/null
      make -j"$JOBS" build_libs >/dev/null
      make install_dev >/dev/null
    )
  fi
  CMAKE_ARGS+=(-DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_USE_STATIC_LIBS=ON)
fi

cmake -S "$SRC/json" -B "$BUILD/json" "${CMAKE_ARGS[@]}" -DJSON_BuildTests=OFF >/dev/null
cmake --build "$BUILD/json" --target install >/dev/null

cmake -S "$SRC/mlspp" -B "$BUILD/mlspp" "${CMAKE_ARGS[@]}" -DTESTING=OFF -DDISABLE_GREASE=ON \
  -DMLS_CXX_NAMESPACE=mlspp -DCMAKE_CXX_FLAGS="-Wno-error" >/dev/null
cmake --build "$BUILD/mlspp" -j"$JOBS" >/dev/null
cmake --install "$BUILD/mlspp" >/dev/null

cmake -S "$SRC/libdave/cpp" -B "$BUILD/libdave" "${CMAKE_ARGS[@]}" -DTESTING=OFF \
  -DCMAKE_CXX_FLAGS="-Wno-error" >/dev/null
cmake --build "$BUILD/libdave" -j"$JOBS" >/dev/null
install -d "$PREFIX/lib" "$PREFIX/include/dave"
install -m644 "$BUILD/libdave/libdave.a" "$PREFIX/lib/"
install -m644 "$SRC/libdave/cpp/includes/dave/"*.h "$PREFIX/include/dave/"

echo "DAVE dependencies installed in $PREFIX"
