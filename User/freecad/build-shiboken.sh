#!/usr/bin/env bash
# Cross-compile the Python binding runtime with the matching host generator.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
c="$root/Kernel/target/user-build"
b="$c/freecad-port"
pyside="$b/pyside-setup-everywhere-src-6.8.3"
qt="$b/qt-gl-build"
qt_host="${QT_HOST_PATH:-$b/qt-host/6.8.3/macos}"
cross=("$c"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$c/gnome-port/mesa-tools/bin:$b/aqt/bin:$PATH"
source "$port/fetch.sh"
fetch_archive "$b/pyside-setup.tar.xz" d2c896f7f1a6a7ea4c9f98cfe317272fd139703ddf612b86999f09e48aeef0e6 \
    https://download.qt.io/official_releases/QtForPython/pyside6/PySide6-6.8.3-src/pyside-setup-everywhere-src-6.8.3.tar.xz
[[ -d "$pyside" ]] || tar xf "$b/pyside-setup.tar.xz" -C "$b"
if ! patch -f -Rs --dry-run -p1 -d "$pyside" < "$port/pyside-static.patch" >/dev/null; then
    patch -f -s -p1 -d "$pyside" < "$port/pyside-static.patch"
fi
generator="${SHIBOKEN_HOST_GENERATOR:-$b/aqt/bin/shiboken6}"
"$generator" --version | grep -q '6.8.3'
# The upstream generator wheel contains the executable but omits its CMake export.
tools="$b/host-shiboken-cmake"
mkdir -p "$tools"
cat > "$tools/Shiboken6ToolsConfig.cmake" <<CMAKE
if(NOT TARGET Shiboken6::shiboken6)
    add_executable(Shiboken6::shiboken6 IMPORTED GLOBAL)
    set_target_properties(Shiboken6::shiboken6 PROPERTIES IMPORTED_LOCATION "$generator")
endif()
CMAKE
cat > "$tools/Shiboken6ToolsConfigVersion.cmake" <<'CMAKE'
set(PACKAGE_VERSION 6.8.3)
if(PACKAGE_FIND_VERSION_MAJOR EQUAL 6 AND NOT PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
endif()
CMAKE
cmake -S "$pyside/sources/shiboken6" -B "$b/shiboken-build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$port/lixinos.cmake" -DCMAKE_BUILD_TYPE=MinSizeRel -DQFP_NO_STRIP=ON -DQt6_DIR="$qt/lib/cmake/Qt6" \
    -DCMAKE_MODULE_PATH="$b/qtbase-everywhere-src-6.8.3/cmake" -DLIXINOS_EXTRA_ROOTS="$qt" -DQT_HOST_PATH="$qt_host" -DShiboken6Tools_DIR="$tools" \
    -DQFP_PYTHON_HOST_PATH="$b/aqt/bin/python" -DPython_INCLUDE_DIR="$b/sdk/include/python3.14" -DPython_LIBRARY="$b/sdk/lib/libpython3.14.a" \
    -DPython_EXECUTABLE="$c/cpython-port/host/python.exe" -DSHIBOKEN_BUILD_TOOLS=OFF -DBUILD_TESTS=OFF \
    -DPYTHON_SITE_PACKAGES=lib/python3.14/site-packages -DPYTHON_EXTENSION_SUFFIX=.abi3 \
    -DPYTHON_CONFIG_SUFFIX=.abi3 -DCMAKE_INSTALL_PREFIX="$b/pyside-sdk"
cmake --build "$b/shiboken-build" --target libshiboken shibokenmodule --parallel "${JOBS:-8}"
cmake --install "$b/shiboken-build"
