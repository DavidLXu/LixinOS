#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
cache=$(cd "$port/../../Kernel/target/user-build" && pwd)
build="$cache/gnome-port"
stage="$build/epoxy-cross"
src="$build/libepoxy-1.5.10"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$cache/gtk3-port/tools/bin:${cross[0]}/bin:$PATH"
[[ -f "$build/sysroot/lib/libEGL.a" ]] || bash "$port/build-mesa.sh"
[[ -d "$src" ]] || tar -xf "$cache/gtk3-port/epoxy.tar.gz" -C "$build"
if ! patch -f -Rs --dry-run -p1 -d "$src" < "$port/epoxy-static-egl.patch" >/dev/null 2>&1; then
    patch -f -s -p1 -d "$src" < "$port/epoxy-static-egl.patch"
fi
if [[ ! -f "$stage/build.ninja" ]]; then
    meson setup "$stage" "$src" --cross-file "$build/cross.ini" --prefix "$build/sysroot" \
        --libdir lib --default-library static --buildtype minsize --wrap-mode nodownload \
        -Dglx=no -Degl=yes -Dx11=false -Dtests=false -Dprefer_static=true
fi
meson compile -C "$stage" -j "${JOBS:-8}"
meson install --no-rebuild -C "$stage"
