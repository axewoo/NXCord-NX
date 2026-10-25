#!/usr/bin/env bash
# Builds the sysmodule and overlay. Run inside devkitpro/devkita64, e.g.
#   docker run --rm -v "$PWD:/src" -w /src devkitpro/devkita64 scripts/build-switch.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITPRO

LIBTESLA_COMMIT=f766e9b607a05e9756843cbd62b3bfb98be1646c
if [ ! -d "$ROOT/deps/src/libtesla/.git" ]; then
  git clone -q https://github.com/WerWolv/libtesla "$ROOT/deps/src/libtesla"
fi
git -C "$ROOT/deps/src/libtesla" checkout -q "$LIBTESLA_COMMIT"

[ -f "$ROOT/deps/switch/lib/libdave.a" ] || "$ROOT/scripts/build-deps.sh" switch

cmake -S "$ROOT" -B "$ROOT/build-switch" -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build "$ROOT/build-switch" -j"$(nproc)"

# SD card layout
OUT=$ROOT/build-switch/sd
rm -rf "$OUT"
mkdir -p "$OUT/atmosphere/contents/5600000000000000" "$OUT/switch/.overlays" "$OUT/config/nxcord"
cp "$ROOT/build-switch/exefs.nsp" "$OUT/atmosphere/contents/5600000000000000/exefs.nsp"
# No boot2.flag: the overlay starts the sysmodule (and can turn on start at boot).
cp "$ROOT/platform/switch/sysmodule/toolbox.json" "$OUT/atmosphere/contents/5600000000000000/toolbox.json"
cp "$ROOT/build-switch/nxcord.ovl" "$OUT/switch/.overlays/nxcord.ovl"
printf 'PASTE_YOUR_BOT_TOKEN_HERE\n' > "$OUT/config/nxcord/token.txt.example"
(cd "$OUT" && rm -f ../NXCord-NX.zip && zip -qr ../NXCord-NX.zip .)
echo "SD files in $OUT, zipped to $ROOT/build-switch/NXCord-NX.zip"
