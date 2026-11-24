#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gnome-port"
prefix="$build/sysroot"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$cache/gtk3-port/tools/bin:${cross[0]}/bin:$PATH"
[[ -f "$build/cross.ini" && -d "$build/libinput-1.27.1" ]] || bash "$port/build-deps.sh"
ar=aarch64-none-elf-ar
jobs=${JOBS:-8}

make -C "$build/libudev-zero-1.0.3" CC="$build/cc" AR="$ar" PREFIX="$prefix" install-static
for name in libevdev-1.13.4 mtdev-1.1.7; do
    stage="$build/$name-build"
    mkdir -p "$stage"
    (cd "$stage" && [[ -f Makefile ]] || CC="$build/cc" AR="$ar" RANLIB=aarch64-none-elf-ranlib \
        "$build/$name/configure" --host=aarch64-unknown-linux-musl --prefix="$prefix" \
            --disable-shared --enable-static)
done

evdev="$build/libevdev-1.13.4-build"
make -C "$evdev/libevdev" -j "$jobs" libevdev.la
install -d "$prefix/include/libevdev-1.0/libevdev"
install -m 644 "$evdev/libevdev/.libs/libevdev.a" "$prefix/lib/"
install -m 644 "$evdev/libevdev.pc" "$prefix/lib/pkgconfig/"
install -m 644 "$build/libevdev-1.13.4/libevdev/libevdev.h" \
    "$build/libevdev-1.13.4/libevdev/libevdev-uinput.h" "$prefix/include/libevdev-1.0/libevdev/"

mtdev="$build/mtdev-1.1.7-build"
make -C "$mtdev/src" -j "$jobs" libmtdev.la
install -m 644 "$mtdev/src/.libs/libmtdev.a" "$prefix/lib/"
install -m 644 "$mtdev/mtdev.pc" "$prefix/lib/pkgconfig/"
install -m 644 "$build/mtdev-1.1.7/include/"mtdev*.h "$prefix/include/"

meson_build() {
    local stage=$1 source=$2
    shift 2
    if [[ -f "$stage/build.ninja" ]]; then
        meson setup --reconfigure "$stage" "$source" "$@"
    else
        meson setup "$stage" "$source" --cross-file "$build/cross.ini" --prefix "$prefix" \
            --libdir lib --default-library static --buildtype minsize --wrap-mode nodownload "$@"
    fi
}

gudev="$build/libgudev-cross"
meson_build "$gudev" "$build/libgudev-238" -Dtests=disabled -Dintrospection=disabled -Dvapi=disabled
meson compile -C "$gudev" -j "$jobs"
meson install --no-rebuild -C "$gudev"

input="$build/libinput-1.27.1"
if ! patch -f -Rs --dry-run -p1 -d "$input" < "$port/libinput-static.patch" >/dev/null 2>&1; then
    patch -f -s -p1 -d "$input" < "$port/libinput-static.patch"
fi
stage="$build/libinput-cross"
meson_build "$stage" "$input" -Dlibwacom=false -Ddebug-gui=false -Dtests=false \
    -Ddocumentation=false -Dzshcompletiondir=no
meson compile -C "$stage" -j "$jobs" input
install -m 644 "$stage/libinput.a" "$prefix/lib/"
install -m 644 "$stage/meson-private/libinput.pc" "$prefix/lib/pkgconfig/"
install -m 644 "$input/src/libinput.h" "$prefix/include/"
