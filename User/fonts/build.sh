#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
work="$cache/fonts-port"
cross=("$cache"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$PATH"
gtk="$cache/gtk3-port"
ft="$cache/lite-xl-port"
net="$cache/netsurf-port"
mkdir -p "$work"
"$cache/gnome-port/cc" -Os -Wall -ffunction-sections -fdata-sections \
    -I"$gtk/sysroot/include" -I"$ft/freetype-VER-2-14-1/include" "$port/fonts.c" \
    -Wl,--start-group -L"$gtk/sysroot/lib" -lfontconfig -lexpat \
    "$ft/freetype-build/libfreetype.a" -L"$net/sysroot/lib" -lpng -lz -lm -Wl,--end-group -o "$work/fonts"
python3 "$port/package.py"
