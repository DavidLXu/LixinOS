#!/usr/bin/env bash
# Add the two Qt source modules used by FreeCAD beyond qtbase.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
b="$cache/freecad-port"
qt="$b/qt-gl-build"
base="$b/qtbase-everywhere-src-6.8.3"
host="${QT_HOST_PATH:-$b/qt-host/6.8.3/macos}"
bash "$port/build-qt-widget.sh" --opengl
cross=("$cache"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$cache/gnome-port/mesa-tools/bin:$PATH"
export PKG_CONFIG_LIBDIR="$cache/wayland-port/sdk/lib/pkgconfig:$b/sdk/lib/pkgconfig:$cache/gnome-port/sysroot/lib/pkgconfig:$cache/gtk3-port/sysroot/lib/pkgconfig"
# External Qt modules look for this install-time template in the target build tree.
install -m 644 "$base/cmake/QtFileConfigure.txt.in" "$qt/lib/cmake/Qt6/"
source "$port/fetch.sh"
build_module() {
    local module=$1 digest=$2 target=$3 out=$4
    shift 4
    local name="$module-everywhere-src-6.8.3"
    fetch_archive "$b/$module.tar.xz" "$digest" "https://download.qt.io/archive/qt/6.8/6.8.3/submodules/$name.tar.xz"
    [[ -d "$b/$name" ]] || tar xf "$b/$module.tar.xz" -C "$b"
    cmake -S "$b/$name" -B "$out" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$port/lixinos.cmake" \
        -DQT_QMAKE_TARGET_MKSPEC=linux-g++ -DQT_MKSPECS_DIR="$base/mkspecs" -DLIXINOS_EXTRA_ROOTS="$qt;$cache/wayland-port/sdk" -UHAVE_EGL \
        -DEGL_STATIC_DEPENDENCIES="$cache/gnome-port/sysroot/lib/libdrm.a;$cache/cpython-port/sysroot/lib/libz.a" \
        -DQT_HOST_PATH="$host" -DQt6_DIR="$qt/lib/cmake/Qt6" -DCMAKE_PREFIX_PATH="$qt" \
        -DCMAKE_MODULE_PATH="$base/cmake" -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX="$b/qt-sdk" \
        -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF "$@"
    cmake --build "$out" --target "$target" --parallel "${JOBS:-8}"
}
build_module qtsvg 35eb516460f00f264eb504baa253432384351cf23fb9980a5857190e8deef438 SvgWidgets "$b/qt-svg-build"
build_module qttools 02a4e219248b94f1333df843d25763f35251c1074cdc4fb5bda67d340f8c8b3a UiTools "$b/qt-tools-build" \
    -DFEATURE_assistant=OFF -DFEATURE_qdoc=OFF -DFEATURE_clang=OFF -DFEATURE_linguist=OFF \
    -DFEATURE_designer=OFF -DFEATURE_qtattributionsscanner=OFF -DFEATURE_pixeltool=OFF \
    -DFEATURE_qtplugininfo=OFF -DFEATURE_distancefieldgenerator=OFF -DFEATURE_qtdiag=OFF
