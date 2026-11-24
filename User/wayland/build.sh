#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/wayland-port"
gtk="$cache/gtk3-port"
musl="$cache/cpython-port"
version=1.24.0
name="wayland-$version"
digest=82892487a01ad67b334eca83b54317a7c86a03a89cfadacfef5211f11a5d0536
archive="$build/$name.tar.xz"
mkdir -p "$build"
if [[ ! -s "$archive" ]]; then
    curl -fLsS --retry 2 "https://gitlab.freedesktop.org/wayland/wayland/-/releases/$version/downloads/$name.tar.xz" -o "$archive.tmp"
    printf '%s  %s\n' "$digest" "$archive.tmp" | shasum -a 256 -c -
    mv "$archive.tmp" "$archive"
fi
printf '%s  %s\n' "$digest" "$archive" | shasum -a 256 -c -
[[ -f "$gtk/sysroot/lib/libffi.a" && -f "$gtk/sysroot/include/ffi.h" ]] || bash "$root/User/gtk3/build.sh"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
LIXINOS_GCC_BUILD="$build/runtime" bash "$root/User/gcc/build.sh" rt

# Each build patches a fresh source directory so upstream files remain reproducible.
work=$(mktemp -d "$build/build.XXXXXX")
trap 'rm -rf "$work"' EXIT
tar xf "$archive" -C "$work"
src="$work/$name"
patch --batch -s -d "$src" -p1 < "$port/wayland.patch"
gen="$work/generated"
mkdir -p "$gen" "$build/package/bin" "$build/package/lib/wayland" \
    "$build/sdk/bin" "$build/sdk/include" "$build/sdk/lib/pkgconfig"
cat > "$gen/config.h" <<'CONFIG'
#define HAVE_ACCEPT4 1
#define HAVE_BROKEN_MSG_CMSG_CLOEXEC 0
#define HAVE_MEMFD_CREATE 1
#define HAVE_MKOSTEMP 1
#define HAVE_POSIX_FALLOCATE 1
#define HAVE_STRNDUP 1
CONFIG
IFS=. read -r major minor micro <<< "$version"
cat > "$gen/wayland-version.h" <<VERSION
#define WAYLAND_VERSION "$version"
#define WAYLAND_VERSION_MAJOR $major
#define WAYLAND_VERSION_MINOR $minor
#define WAYLAND_VERSION_MICRO $micro
VERSION
python3 "$src/src/embed.py" "$src/protocol/wayland.dtd" wayland_dtd > "$gen/wayland.dtd.h"
cc -O2 -I"$gen" -include "$gen/config.h" "$src/src/scanner.c" "$src/src/wayland-util.c" \
    -lexpat -o "$work/wayland-scanner"
for side in server client; do
    "$work/wayland-scanner" -s "$side-header" "$src/protocol/wayland.xml" "$gen/wayland-$side-protocol.h"
    "$work/wayland-scanner" -s -c "$side-header" "$src/protocol/wayland.xml" "$gen/wayland-$side-protocol-core.h"
done
"$work/wayland-scanner" -s public-code "$src/protocol/wayland.xml" "$gen/wayland-protocol.c"

cc="$musl/cc"
flags=(-Os -std=gnu11 -D__lixinos__ -ffunction-sections -fdata-sections -I"$gen" -I"$src/src" -I"$gtk/sysroot/include" -include "$gen/config.h")
compile() {
    local source=$1
    shift
    "$cc" "${flags[@]}" "$@" -c "$source" -o "$work/$(basename "${source%.c}").o"
}
for unit in wayland-util connection wayland-os wayland-shm wayland-server wayland-client; do
    compile "$src/src/$unit.c"
done
compile "$src/src/event-loop.c" -D_GNU_SOURCE -include "$port/poll-backend.h" -include "$port/lixin-wayland-loop.h"
compile "$port/poll-backend.c"
compile "$gen/wayland-protocol.c"
common=("$work"/{wayland-util,connection,wayland-os,wayland-protocol}.o)
ar=aarch64-none-elf-ar
"$ar" crsD "$work/libwayland-client.a" "${common[@]}" "$work/wayland-client.o"
"$ar" crsD "$work/libwayland-server.a" "${common[@]}" "$work"/{wayland-server,wayland-shm,event-loop,poll-backend}.o
for unit in wayland-cursor os-compatibility xcursor; do
    compile "$src/cursor/$unit.c" '-DICONDIR="/share/icons"'
done
"$ar" crsD "$work/libwayland-cursor.a" "$work"/{wayland-cursor,os-compatibility,xcursor}.o
compile "$src/egl/wayland-egl.c"
"$ar" crsD "$work/libwayland-egl.a" "$work/wayland-egl.o"

"$cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I"$root/User/gcc" \
    -c "$root/User/cpython/loader.c" -o "$work/crt0.o"
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$work/crt1.o"
for side in server client; do
    unit=demo
    [[ $side == server ]] || unit=shm-client
    compile "$port/$unit.c"
    "$cc" -nostartfiles -static -s -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
        -Wl,-T,"$root/User/gcc/application.ld" "$work/crt0.o" "$work/crt1.o" "$work/$unit.o" \
        -Wl,--start-group "$work/libwayland-$side.a" "$gtk/sysroot/lib/libffi.a" \
        "$build/runtime/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group -o "$build/package/bin/wayland-$unit"
done
install -m 644 "$work"/libwayland-*.a "$build/sdk/lib/"
install -m 644 "$gtk/sysroot/lib/libffi.a" "$build/sdk/lib/"
install -m 755 "$work/wayland-scanner" "$build/sdk/bin/"
install -m 644 "$gen/wayland-version.h" "$gen"/wayland-{client,server}-protocol.h \
    "$src/src"/wayland-{client,server,client-core,server-core,util}.h \
    "$src/cursor/wayland-cursor.h" "$src/egl"/wayland-egl{,-core,-backend}.h \
    "$port/lixin-wayland-loop.h" "$build/sdk/include/"
install -m 644 "$src/COPYING" "$build/package/lib/wayland/COPYING"
install -m 644 "$src/protocol/wayland.xml" "$build/sdk/"
for side in server client cursor egl egl-backend scanner; do
    api_version=$version
    libs="-L\${libdir} -lwayland-$side"
    private=-lffi
    requires=
    case $side in
        egl) api_version=18.1.0; requires=wayland-client; private= ;;
        cursor) requires=wayland-client; private= ;;
        egl-backend) api_version=3; libs=; private= ;;
        scanner) libs=; private= ;;
    esac
    cat > "$build/sdk/lib/pkgconfig/wayland-$side.pc" <<PC
prefix=\${pcfiledir}/../..
libdir=\${prefix}/lib
includedir=\${prefix}/include
wayland_scanner=\${prefix}/bin/wayland-scanner

Name: Wayland $side
Description: Wayland $side library for LixinOS
Version: $api_version
Requires: $requires
Libs: $libs
Libs.private: $private
Cflags: -I\${includedir}
PC
done
echo "Wayland package: $build/package"
