#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/freecad-port"
mkdir -p "$build/obj"
[[ -x "$cache/gnome-port/cxx" && -f "$cache/gnome-port/runtime/rt/liblx.a" ]] || bash "$root/User/gnome/build-deps.sh"
cross=("$cache"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$cache/gtk3-port/tools/bin:$PATH"
source "$port/fetch.sh"
commit=4fd3bf320d9566a27e60069fc8387448aaa3a094
fetch_archive "$build/FreeCAD.tar.gz" d381064aede10f7d9388bd78d2073d05f07b4da53f723940a16860e0c5fe446a "https://codeload.github.com/FreeCAD/FreeCAD/tar.gz/$commit"
fetch_archive "$build/occt.tar.gz" 3cd080d3fc33ba0c6c157e110afe3e015859524c4694dbb09812ec9d61595639 https://codeload.github.com/Open-Cascade-SAS/OCCT/tar.gz/refs/tags/V7_9_2
fetch_archive "$build/eigen-dev-3.4.0-r10.apk" e39d788417543f7ba14bb9cef951447f122bd71f8ae229fe77212d7d5cf990af https://dl-cdn.alpinelinux.org/alpine/v3.23/community/aarch64/eigen-dev-3.4.0-r10.apk
fetch_archive "$build/boost-headers.apk" 8aae3df528b54b62c653a38b41076496f560eb5baa96f0e816ace1872d99451a https://dl-cdn.alpinelinux.org/alpine/v3.23/main/aarch64/boost1.84-dev-1.84.0-r3.apk
[[ -d "$build/FreeCAD-$commit" ]] || tar xf "$build/FreeCAD.tar.gz" -C "$build"
occt="$build/OCCT-7_9_2"
[[ -d "$occt" ]] || tar xf "$build/occt.tar.gz" -C "$build"
mkdir -p "$build/eigen" "$build/boost"
[[ -f "$build/eigen/usr/include/eigen3/Eigen/Core" ]] || tar xf "$build/eigen-dev-3.4.0-r10.apk" -C "$build/eigen" usr/include
[[ -f "$build/boost/usr/include/boost/version.hpp" ]] || tar xf "$build/boost-headers.apk" -C "$build/boost" usr/include
if ! patch -Rs --dry-run -f -p1 -d "$occt" < "$port/occt-stacktrace.patch"; then
    patch -s -f -p1 -d "$occt" < "$port/occt-stacktrace.patch"
fi
cmake -S "$occt" -B "$build/occt-build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$port/lixinos.cmake" -DCMAKE_INSTALL_PREFIX="$build/sdk" \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_LIBRARY_TYPE=Static -DBUILD_MODULE_Draw=OFF \
    -DBUILD_MODULE_DETools=OFF -DBUILD_MODULE_DataExchange=OFF -DBUILD_MODULE_ApplicationFramework=OFF \
    -DBUILD_MODULE_Visualization=OFF -DUSE_FREETYPE=OFF -DUSE_TCL=OFF -DUSE_TK=OFF -DUSE_OPENGL=OFF -DUSE_XLIB=OFF
cmake --build "$build/occt-build" --target TKBO TKPrim --parallel "${JOBS:-8}"
make -f "$port/sketcher.mk" -j "${JOBS:-8}" build="$build" port="$port" AR=aarch64-none-elf-ar \
    CC="$cache/gnome-port/cc" CXX="$cache/gnome-port/cxx" "$build/sketch-probe" "$build/part-window"
python3 "$port/package.py"
