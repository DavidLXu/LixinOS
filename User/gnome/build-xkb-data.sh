#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gnome-port"
export PATH="$cache/gtk3-port/tools/bin:$PATH"
[[ -d "$build/xkeyboard-config-2.45" ]] || bash "$port/build-deps.sh"
if [[ ! -f "$build/xkb-data-native/build.ninja" ]]; then
    meson setup "$build/xkb-data-native" "$build/xkeyboard-config-2.45" \
        --prefix "$build/xkb-data" -Dnls=false --wrap-mode nodownload
fi
meson compile -C "$build/xkb-data-native"
meson install --no-rebuild -C "$build/xkb-data-native"
