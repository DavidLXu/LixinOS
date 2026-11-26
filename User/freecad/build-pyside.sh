#!/usr/bin/env bash
# Generate and compile the actual Qt Python modules used by the Part/Sketcher UI.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
c="$root/Kernel/target/user-build"
b="$c/freecad-port"
sdk="$b/pyside-sdk"
qt="$b/qt-gl-build"
qt_host="${QT_HOST_PATH:-$b/qt-host/6.8.3/macos}"
cross=("$c"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$c/gnome-port/mesa-tools/bin:$b/aqt/bin:$PATH"
export PKG_CONFIG_LIBDIR="$c/wayland-port/sdk/lib/pkgconfig:$b/sdk/lib/pkgconfig:$c/gnome-port/sysroot/lib/pkgconfig:$c/gtk3-port/sysroot/lib/pkgconfig:$c/cpython-port/sysroot/lib/pkgconfig"
[[ -f "$sdk/lib/libshiboken6.abi3.a" ]] || bash "$port/build-shiboken.sh"
# Shiboken asks this compiler for the target include order. libstdc++ uses
# include_next for the musl C headers, so those must follow the C++ headers.
cpp="$c/gnome-port/alpine-sdk/usr/include/c++/14.2.0"
probe="$b/shiboken-target-compiler"
cat > "$probe" <<WRAPPER
#!/bin/sh
exec "${cross[0]}/aarch64-none-elf-g++" -nostdinc -nostdinc++ -isystem "$cpp" -isystem "$cpp/aarch64-alpine-linux-musl" -isystem "$c/cpython-port/sysroot/include" "\$@"
WRAPPER
chmod +x "$probe"
cmake -S "$b/pyside-setup-everywhere-src-6.8.3/sources/pyside6" -B "$b/pyside-build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$port/lixinos.cmake" -DCMAKE_BUILD_TYPE=MinSizeRel -DQFP_NO_STRIP=ON "-DCMAKE_CXX_FLAGS=-D_GNU_SOURCE -D_LARGEFILE64_SOURCE -I$qt/include/QtGui/6.8.3/QtGui/rhi -I$b/sdk/include -I$c/gnome-port/sysroot/include" -DCMAKE_INSTALL_PREFIX="$sdk" \
    -DQt6_DIR="$qt/lib/cmake/Qt6" -DQFP_QT_TARGET_PATH="$qt" -DQT_HOST_PATH="$qt_host" -DQFP_QT_HOST_PATH="$qt_host" \
    -DCMAKE_MODULE_PATH="$b/qtbase-everywhere-src-6.8.3/cmake" \
    "-DLIXINOS_EXTRA_ROOTS=$qt;$sdk;$b/qt-tools-build;$b/qt-svg-build;$c/wayland-port/sdk" \
    -DShiboken6_DIR="$sdk/lib/cmake/Shiboken6" -DShiboken6Tools_DIR="$b/host-shiboken-cmake" \
    -DQFP_PYTHON_HOST_PATH="$b/aqt/bin/python" -DPython_INCLUDE_DIR="$b/sdk/include/python3.14" \
    -DPython_LIBRARY="$b/sdk/lib/libpython3.14.a" -DPython_EXECUTABLE="$c/cpython-port/host/python.exe" \
    -DPYTHON_SITE_PACKAGES=lib/python3.14/site-packages -DPYTHON_CONFIG_SUFFIX=.abi3 -DPYTHON_EXTENSION_SUFFIX=.abi3 \
    -DEGL_STATIC_DEPENDENCIES="$c/gnome-port/sysroot/lib/libdrm.a;$c/cpython-port/sysroot/lib/libz.a" \
    "-DQT_ADDITIONAL_PACKAGES_PREFIX_PATH=$b/qt-tools-build;$b/qt-svg-build" \
    -DQt6UiTools_DIR="$b/qt-tools-build/lib/cmake/Qt6UiTools" -DBUILD_TESTS=OFF \
    "-DSHIBOKEN_GENERATOR_EXTRA_FLAGS=--platform=unix;--compiler=g++;--compiler-path=$probe;--clang-option=--target=aarch64-linux-musl;--clang-option=-I$b/sdk/include;--clang-option=-I$c/gnome-port/sysroot/include;--clang-option=-I$b/qt-tools-build/include;--clang-option=-I$b/qt-svg-build/include" \
    "-DMODULES=Core;Gui;Widgets;UiTools;PrintSupport"
cmake --build "$b/pyside-build" --target QtCore QtGui QtWidgets QtUiTools QtPrintSupport --parallel "${JOBS:-8}"
