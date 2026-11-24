#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/dbus-port"
gtk="$cache/gtk3-port"
version=1.14.10
digest=ba1f21d2bd9d339da2d4aa8780c09df32fea87998b73da24f49ab9df1e36a50f
name="dbus-$version"
archive="$build/$name.tar.xz"
mkdir -p "$build"
if [[ ! -s "$archive" ]]; then
    curl -fLsS --retry 2 "https://dbus.freedesktop.org/releases/dbus/$name.tar.xz" -o "$archive.tmp"
    printf '%s  %s\n' "$digest" "$archive.tmp" | shasum -a 256 -c -
    mv "$archive.tmp" "$archive"
fi
printf '%s  %s\n' "$digest" "$archive" | shasum -a 256 -c -
if [[ ! -d "$build/$name" ]]; then
    tar -xf "$archive" -C "$build"
fi
src="$build/$name"
if ! patch -f -Rs --dry-run -d "$src" -p1 < "$port/dbus.patch" >/dev/null 2>&1; then
    patch --batch -s -d "$src" -p1 < "$port/dbus.patch"
fi
[[ -f "$gtk/sysroot/lib/libexpat.a" ]] || bash "$root/User/gtk3/build.sh"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$gtk/tools/bin:${cross[0]}/bin:$PATH"
LIXINOS_GCC_BUILD="$build/runtime" bash "$root/User/gcc/build.sh" rt
export PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR="$gtk/sysroot/lib/pkgconfig"

stage="$build/build"
cmake -S "$src" -B "$stage" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_C_COMPILER="$build/runtime/cc" \
    -DCMAKE_AR="$(command -v aarch64-none-elf-ar)" -DCMAKE_RANLIB="$(command -v aarch64-none-elf-ranlib)" \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_C_FLAGS="-ffunction-sections -fdata-sections" \
    -DCMAKE_INSTALL_PREFIX=/ -DCMAKE_INSTALL_LIBDIR=lib -DDBUS_SESSION_SOCKET_DIR=/tmp \
    -DDBUS_BUILD_TESTS=OFF -DDBUS_BUILD_X11=OFF -DCMAKE_DISABLE_FIND_PACKAGE_X11=ON \
    -DDBUS_WITH_GLIB=OFF -DDBUS_BUS_ENABLE_INOTIFY=OFF -DENABLE_SYSTEMD=OFF \
    -DDBUS_RELOCATABLE=OFF -DDBUS_ENABLE_VERBOSE_MODE=OFF -DENABLE_TRADITIONAL_ACTIVATION=OFF \
    -DEXPAT_INCLUDE_DIR="$gtk/sysroot/include" -DEXPAT_LIBRARY="$gtk/sysroot/lib/libexpat.a"
cmake --build "$stage" --parallel "${JOBS:-8}" --target dbus-daemon dbus-send dbus-uuidgen
"$build/runtime/cc" -Os -ffunction-sections -fdata-sections \
    -I"$cache/cpython-port/musl-1.2.5/src/process" "$port/run-session.c" "$root/User/rust/spawn.c" \
    -o "$stage/bin/dbus-run-session"

package="$build/package"
sdk="$build/sdk"
install -d "$package/lib/dbus" "$sdk/include/dbus" "$sdk/lib/pkgconfig"
install -m 644 "$port/session.conf" "$src/COPYING" "$package/lib/dbus/"
python3 - "$root" "$stage" "$package" <<'PY'
from pathlib import Path
import runpy
import sys

root, stage, package = map(Path, sys.argv[1:])
tool = runpy.run_path(str(root / "User/gcc/package.py"))["tool"]
for name in ("dbus-daemon", "dbus-send", "dbus-uuidgen", "dbus-run-session"):
    tool(package, "/bin/" + name, stage / "bin" / name, library="/lib/dbus",
         launcher_flag="-DLX_DBUS", build_dir=stage, deflate=True)
PY
install -m 644 "$src"/dbus/*.h "$stage/dbus/dbus-arch-deps.h" "$sdk/include/dbus/"
install -m 644 "$stage/lib/libdbus-1.a" "$sdk/lib/"
cat > "$sdk/lib/pkgconfig/dbus-1.pc" <<PC
prefix=\${pcfiledir}/../..
Name: dbus
Description: D-Bus client library for LixinOS
Version: $version
Libs: -L\${prefix}/lib -ldbus-1
Cflags: -I\${prefix}/include
PC
echo "D-Bus package: $package"
