#!/usr/bin/env bash
# Builds nxcord-cli for testing on a Linux PC.
# Arch/CachyOS: sudo pacman -S --needed base-devel cmake git mbedtls libsodium opus openssl
# Debian/Ubuntu: sudo apt install build-essential cmake git libmbedtls-dev libsodium-dev libopus-dev libssl-dev
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$ROOT/deps/linux/lib/libdave.a" ] || "$ROOT/scripts/build-deps.sh" linux
cmake -S "$ROOT" -B "$ROOT/build-linux" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$ROOT/build-linux" -j"$(nproc)"
echo "Built $ROOT/build-linux/platform/linux/nxcord-cli"
