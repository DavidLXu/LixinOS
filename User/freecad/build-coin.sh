#!/usr/bin/env bash
# Build the scene graph used by FreeCAD's 3D view, against the guest GL SDK.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/freecad-port"
g="$cache/gnome-port"
sdk="$build/sdk"
bash "$port/build-gl.sh"
[[ -f "$build/boost/usr/include/boost/version.hpp" ]] || bash "$port/build.sh"
cross=("$cache"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$g/mesa-tools/bin:$PATH"
source "$port/fetch.sh"
fetch_archive "$build/coin.tar.gz" 086ecf84479e4bc59397568638488c2e6c08d8aa811779bab93cda5509f79d59 https://codeload.github.com/coin3d/coin/tar.gz/refs/tags/v4.0.3
src="$build/coin-4.0.3"
[[ -d "$src" ]] || tar xf "$build/coin.tar.gz" -C "$build"
if ! patch -f -Rs --dry-run -p1 -d "$src" < "$port/coin-egl.patch" >/dev/null; then
    patch -f -s -p1 -d "$src" < "$port/coin-egl.patch"
fi
flags="-D_GNU_SOURCE -I$sdk/include -I$g/sysroot/include"
cmake -S "$src" -B "$build/coin-build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$port/lixinos.cmake" \
    -DCMAKE_INSTALL_PREFIX="$sdk" -DCMAKE_BUILD_TYPE=MinSizeRel -DCOIN_BUILD_SHARED_LIBS=OFF \
    -DCOIN_EGL_LIBRARY="${FREECAD_EGL_LIBRARY:-$g/sysroot/lib/libEGL.a}" \
    -DCOIN_BUILD_TESTS=OFF -DCOIN_USE_EGL=ON -DGLU_RUNTIME_LINKING=OFF \
    -DBoost_INCLUDE_DIR="$build/boost/usr/include" -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags"
cmake --build "$build/coin-build" --parallel "${JOBS:-8}"
cmake --install "$build/coin-build"
"$g/cxx" -Os -DTEST_COIN "$port/test-gl.cpp" "$port/test-coin.cpp" -I"$sdk/include" -I"$g/sysroot/include" \
    -L"$sdk/lib" -L"$g/sysroot/lib" -Wl,--start-group -lCoin -lGLU -lGL -lEGL -ldrm -lz -Wl,--end-group -o "$build/test-coin"
