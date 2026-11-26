#!/usr/bin/env bash
# Build the upstream application and the workbenches linked into its executable.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
cross=("$cache"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$cache/gnome-port/mesa-tools/bin:$cache/freecad-port/aqt/bin:$PATH"
b="$cache/freecad-port"
if ! grep -Fq "$b/wayland-egl-sdk/lib/libEGL.a" "$b/sdk/lib/cmake/Coin-4.0.3/coin-export.cmake"; then
    FREECAD_EGL_LIBRARY="$b/wayland-egl-sdk/lib/libEGL.a" bash "$port/build-coin.sh"
fi
bash "$port/configure-desktop.sh"
cmake --build "$cache/freecad-port/original-desktop" --target FreeCADMain --parallel "${JOBS:-8}"
