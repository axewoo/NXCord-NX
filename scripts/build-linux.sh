#!/usr/bin/env bash
# Builds nxcord-cli for testing on a Linux PC.
# Arch/CachyOS: sudo pacman -S --needed base-devel cmake git libsodium opus openssl
# Debian/Ubuntu: sudo apt install build-essential cmake git libsodium-dev libopus-dev libssl-dev
# (mbedTLS, mlspp and libdave are built into deps/linux by build-deps.sh)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$ROOT/deps/linux/lib/libdave.a" ] && [ -f "$ROOT/deps/linux/lib/libmbedtls.a" ] || "$ROOT/scripts/build-deps.sh" linux
cmake -S "$ROOT" -B "$ROOT/build-linux" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$ROOT/build-linux" -j"$(nproc)"
echo "Built $ROOT/build-linux/platform/linux/nxcord-cli"
