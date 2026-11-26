#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
b="$cache/freecad-port"
[[ -d "$b/OCCT-7_9_2" ]] || bash "$port/build.sh"
cross=("$cache"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$cache/gtk3-port/tools/bin:$PATH"
cmake -S "$b/OCCT-7_9_2" -B "$b/occt-desktop" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$port/lixinos.cmake" \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_LIBRARY_TYPE=Static -DBUILD_MODULE_Draw=OFF -DBUILD_MODULE_DETools=ON \
    -DUSE_FREETYPE=OFF -DUSE_TCL=OFF -DUSE_TK=OFF -DUSE_OPENGL=OFF -DUSE_XLIB=OFF -DUSE_RAPIDJSON=OFF
cmake --build "$b/occt-desktop" --target TKDEGLTF TKDEIGES TKDESTEP TKDESTL TKExpress TKFeat TKFillet TKOffset TKHLR \
    --parallel "${JOBS:-8}"
