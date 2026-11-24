#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
cache=$(cd "$port/../../Kernel/target/user-build" && pwd)
build="$cache/gnome-port"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$cache/gtk3-port/tools/bin:${cross[0]}/bin:$PATH"
[[ -x "$build/shell-cross/src/gnome-shell" ]] || bash "$port/build-shell.sh"
for name in accountsservice gdm gweather-4 ibus-1.0 geoclue-2 upower-glib; do
    if [[ ! -s "$build/sysroot/lib/lib$name.a" ]]; then
        bash "$port/build-shell-clients.sh"
        break
    fi
done
bash "$port/prepare-shell-metadata.sh"
ninja -C "$build/shell-cross" -j "${JOBS:-8}" \
    data/gnome-shell-dbus-interfaces.gresource data/gnome-shell-icons.gresource \
    data/gnome-shell-osk-layouts.gresource data/gnome-shell-theme.gresource
python3 "$port/link-shell-runtime.py"
