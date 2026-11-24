#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gnome-port"
prefix="$build/sysroot"
src="$build/pulseaudio-17.0"
stage="$build/pulse-cross"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$cache/gtk3-port/tools/bin:${cross[0]}/bin:$PATH"
[[ -d "$src" ]] || bash "$port/build-deps.sh"
if ! patch -f -Rs --dry-run -p1 -d "$src" < "$port/pulse-client.patch" >/dev/null 2>&1; then
    patch -f -s -p1 -d "$src" < "$port/pulse-client.patch"
fi
if [[ ! -f "$stage/build.ninja" ]]; then
    meson setup "$stage" "$src" --cross-file "$build/cross.ini" --prefix "$prefix" \
        --libdir lib --default-library static --buildtype minsize --wrap-mode nodownload \
        -Dprefer_static=true -Ddaemon=false -Dclient=true -Dtests=false -Ddoxygen=false -Dman=false \
        -Dauto_features=disabled -Dglib=enabled -Ddatabase=simple
fi
ninja -C "$stage" -j "${JOBS:-8}" src/pulse/libpulse.a src/pulse/libpulse-mainloop-glib.a src/libpulsecommon-17.0.a
install -d "$prefix/lib/pulseaudio" "$prefix/include/pulse"
install -m 644 "$stage/src/pulse/libpulse.a" "$stage/src/pulse/libpulse-mainloop-glib.a" "$prefix/lib/"
install -m 644 "$stage/src/libpulsecommon-17.0.a" "$prefix/lib/pulseaudio/"
install -m 644 "$src/src/pulse/"*.h "$stage/src/pulse/version.h" "$prefix/include/pulse/"
install -m 644 "$stage/libpulse.pc" "$stage/libpulse-mainloop-glib.pc" "$prefix/lib/pkgconfig/"
