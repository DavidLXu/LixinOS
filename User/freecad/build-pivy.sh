#!/usr/bin/env bash
# Build the upstream Coin Python binding for embedding in FreeCAD.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
c="$root/Kernel/target/user-build"
b="$c/freecad-port"
commit=6cfa731cbcdae2866edf7d84f4b9e68c5b1a70ef
src="$b/pivy-$commit"
out="$b/pivy-build"
source "$port/fetch.sh"
fetch_archive "$b/pivy.tar.gz" af8ee8b3e63d00472dd0b1ababae66b9c34cc688dc378c6ac94bd3bbb69ead0c \
    "https://codeload.github.com/coin3d/pivy/tar.gz/$commit"
[[ -d "$src" ]] || tar xf "$b/pivy.tar.gz" -C "$b"
cross=("$c"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$b/aqt/bin:$PATH"
mkdir -p "$out/pivy"
cp -R "$src/pivy/" "$out/pivy/"
python3 "$src/install_helpers.py" "$src" "$b/sdk/include"
swig -c++ -python -includeall -D__PIVY__ -w302,306,307,312,314,325,361,362,467,389,503,509,510 \
    -I"$src" -I"$src/fake_headers" -I"$b/sdk/include" -outdir "$out/pivy" \
    -o "$out/coin_wrap.cpp" "$src/interfaces/coin.i"
"$c/gnome-port/cxx" -Os -ffunction-sections -fdata-sections -std=c++17 -D_GNU_SOURCE \
    -I"$src/interfaces" -I"$b/sdk/include" -I"$c/gnome-port/sysroot/include" -I"$b/sdk/include/python3.14" \
    -c "$out/coin_wrap.cpp" -o "$out/coin_wrap.o"
aarch64-none-elf-ar rcs "$out/libpivy.a" "$out/coin_wrap.o"
