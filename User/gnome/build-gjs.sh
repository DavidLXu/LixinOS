#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gnome-port"
prefix="$build/sysroot"
src="$build/gjs-1.86.0"
stage="$build/gjs-cross"

[[ -d "$src" ]] || bash "$port/build-deps.sh"
[[ -f "$prefix/lib/libmozjs-140.a" ]] || bash "$port/build-mozjs.sh"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$cache/gtk3-port/tools/bin:${cross[0]}/bin:$PATH"
if ! patch -f -Rs --dry-run -p1 -d "$src" < "$port/gjs-static.patch" >/dev/null 2>&1; then
    patch -f -s -p1 -d "$src" < "$port/gjs-static.patch"
fi
options=(-Dprefer_static=true -Dinstalled_tests=false -Dskip_gtk_tests=true -Dskip_dbus_tests=true
         -Dprofiler=disabled -Dreadline=disabled -Dbsymbolic_functions=false -Db_pch=false)
if [[ -f "$stage/build.ninja" ]]; then
    cache_options=(--reconfigure)
    if [[ "$prefix/lib/pkgconfig/mozjs-140.pc" -nt "$stage/build.ninja" ]]; then
        cache_options+=(--clearcache)
    fi
    meson setup "${cache_options[@]}" "$stage" "$src" "${options[@]}"
else
    meson setup "$stage" "$src" --cross-file "$build/cross.ini" --prefix "$prefix" --libdir lib \
        --default-library static --buildtype minsize --wrap-mode nodownload "${options[@]}"
fi
meson compile -C "$stage" -j "${JOBS:-8}"
meson install --no-rebuild -C "$stage"
pc="$prefix/lib/pkgconfig/gjs-1.0.pc"
sed '/^Cflags:/s/ -DGJS_COMPILATION.*//' "$pc" > "$pc.tmp"
mv "$pc.tmp" "$pc"
