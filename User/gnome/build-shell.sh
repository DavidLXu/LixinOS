#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gnome-port"
prefix="$build/sysroot"
src="$build/gnome-shell-49.0"
stage="$build/shell-cross"
wayland="$cache/wayland-port/sdk"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$wayland/bin:$cache/gtk3-port/tools/bin:${cross[0]}/bin:$PATH"
export PKG_CONFIG_PATH="$wayland/lib/pkgconfig"
[[ -d "$src" ]] || bash "$port/build-deps.sh"
[[ -f "$prefix/lib/libmutter-17.a" ]] || bash "$port/build-mutter.sh"
[[ -f "$prefix/lib/libgjs.a" ]] || bash "$port/build-gjs.sh"
[[ -f "$prefix/lib/libgtk-4.a" ]] || bash "$port/build-gtk4.sh"
[[ -f "$prefix/lib/libgnome-desktop-4.a" ]] || bash "$port/build-shell-libs.sh"
[[ -f "$prefix/lib/libpulse.a" ]] || bash "$port/build-pulse-client.sh"

if ! patch -f -Rs --dry-run -p1 -d "$src" < "$port/shell-static.patch" >/dev/null 2>&1; then
    patch -f -s -p1 -d "$src" < "$port/shell-static.patch"
fi
if ! patch -f -Rs --dry-run -p1 -d "$src" < "$port/shell-greeter.patch" >/dev/null 2>&1; then
    patch -f -s -p1 -d "$src" < "$port/shell-greeter.patch"
fi
LIXINOS_GCC_BUILD="$build/runtime" bash "$root/User/gcc/build.sh" rt

options=(-Dprefer_static=true -Dcamera_monitor=false -Dsystemd=false -Dtests=false -Dman=false
         -Dnetworkmanager=false -Dportal_helper=false -Dextensions_app=false -Dextensions_tool=false)
if [[ -f "$stage/build.ninja" ]]; then
    cache_options=(--reconfigure)
    if [[ "$prefix/lib/pkgconfig/mozjs-140.pc" -nt "$stage/build.ninja" ]]; then
        cache_options+=(--clearcache)
    fi
    meson setup "${cache_options[@]}" "$stage" "$src" "${options[@]}"
else
    meson setup "$stage" "$src" --cross-file "$build/cross.ini" --prefix /lib/gnome --libdir lib \
        --default-library static --buildtype minsize --wrap-mode nodownload "${options[@]}"
fi
meson compile -C "$stage" -j "${JOBS:-8}" gnome-shell
