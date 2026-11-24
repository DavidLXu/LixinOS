#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/mujoco-port"
mkdir -p "$build/obj" "$build/include/ccd" "$cache/mujoco-package/bin" "$cache/mujoco-package/lib/mujoco"
fetch() {
    local archive=$1 url=$2 digest=$3 directory=$4
    if [[ ! -f "$build/$archive" ]]; then
        curl -fLsS --retry 3 "$url" -o "$build/$archive.tmp"
        mv "$build/$archive.tmp" "$build/$archive"
    fi
    printf '%s  %s\n' "$digest" "$build/$archive" | shasum -a 256 -c -
    [[ -d "$build/$directory" ]] || tar xf "$build/$archive" -C "$build"
}
ccd_commit=7931e764a19ef6b21b443376c699bbc9c6d4fba8
fetch 4.0.1.tar.gz https://codeload.github.com/google-deepmind/mujoco/tar.gz/refs/tags/4.0.1 a1ccbe11674d09c4201067fca4d9954b1e4b0904cccdca991e21428e73d7db43 mujoco-4.0.1
fetch ccd.tar.gz "https://codeload.github.com/danfis/libccd/tar.gz/$ccd_commit" 479994a86d32e2effcaad64204142000ee6b6b291fd1859ac6710aee8d00a482 "libccd-$ccd_commit"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
export PORT="$port" BUILD="$build" SRC="$build/mujoco-4.0.1" CCD="$build/libccd-$ccd_commit"
export MUSL="$cache/cpython-port" ALPINE="$cache/gnome-port/alpine-sdk/usr"
export RT="$cache/gnome-port/runtime/rt/liblx.a"
[[ -f "$ALPINE/lib/libstdc++.a" && -f "$RT" ]] || bash "$root/User/gnome/build-deps.sh"
[[ -f "$build/include/ccd/config.h" ]] || printf '#define CCD_DOUBLE\n' > "$build/include/ccd/config.h"
make -f "$port/Makefile" -j "${JOBS:-10}" "$build/mujoco"
aarch64-none-elf-strip -o "$cache/mujoco-package/bin/mujoco" "$build/mujoco"
install -m 644 "$port/pendulum.mjb" "$cache/mujoco-package/lib/mujoco/"
install -m 644 "$SRC/LICENSE" "$cache/mujoco-package/lib/mujoco/LICENSE-MuJoCo"
install -m 644 "$CCD/BSD-LICENSE" "$cache/mujoco-package/lib/mujoco/LICENSE-libccd"
