#!/usr/bin/env bash
# Build the Qt Wayland client and an upstream widget client for guest Mutter.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
c="$root/Kernel/target/user-build"
b="$c/freecad-port"
q="$b/qt-gl-build"
g="$c/gnome-port"
w="$c/wayland-port/sdk"
host="${QT_HOST_PATH:-$b/qt-host/6.8.3/macos}"
base="$b/qtbase-everywhere-src-6.8.3"
src="$b/qtwayland-everywhere-src-6.8.3"
out="$b/qt-wayland-build"
qttools="$b/wayland-scanner-host"
cross=("$c"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$w/bin:$g/mesa-tools/bin:$PATH"
export PKG_CONFIG_LIBDIR="$w/lib/pkgconfig:$b/sdk/lib/pkgconfig:$g/sysroot/lib/pkgconfig:$c/gtk3-port/sysroot/lib/pkgconfig"
source "$port/fetch.sh"
fetch_archive "$b/qtwayland.tar.xz" 20fe385887d21190165a3180c17dcfc8b9a0e1da4ec76865b6334bdc709994b0 \
    https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtwayland-everywhere-src-6.8.3.tar.xz
[[ -d "$src" ]] || tar xf "$b/qtwayland.tar.xz" -C "$b"
if ! patch -f -Rs --dry-run -p1 -d "$src" < "$port/qt-wayland-keymap.patch" >/dev/null; then
    patch -f -s -p1 -d "$src" < "$port/qt-wayland-keymap.patch"
fi
# Only the XML code generator is built with the host compiler.
cmake -S "$src" -B "$qttools" -G Ninja -DCMAKE_PREFIX_PATH="$host;$w" -DQt6_DIR="$host/lib/cmake/Qt6" \
    -DWaylandScanner_EXECUTABLE="$w/bin/wayland-scanner" -DCMAKE_BUILD_TYPE=Release -DQT_BUILD_TESTS=OFF \
    -DQT_BUILD_EXAMPLES=OFF -DQT_FORCE_WARN_APPLE_SDK_AND_XCODE_CHECK=ON -DFEATURE_wayland_client=OFF -DFEATURE_wayland_server=ON
cmake --build "$qttools" --target qtwaylandscanner
cmake -S "$src" -B "$out" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$port/lixinos.cmake" -DQT_QMAKE_TARGET_MKSPEC=linux-g++ \
    -DQT_MKSPECS_DIR="$base/mkspecs" -DLIXINOS_EXTRA_ROOTS="$q;$w" -DQT_HOST_PATH="$host" \
    -DQt6WaylandScannerTools_DIR="$qttools/lib/cmake/Qt6WaylandScannerTools" -DWaylandScanner_EXECUTABLE="$w/bin/wayland-scanner" \
    -DQt6_DIR="$q/lib/cmake/Qt6" -DCMAKE_PREFIX_PATH="$q;$w" -DCMAKE_MODULE_PATH="$base/cmake" \
    -DEGL_STATIC_DEPENDENCIES="$g/sysroot/lib/libdrm.a;$c/cpython-port/sysroot/lib/libz.a" \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX="$b/qt-sdk" -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF -DFEATURE_wayland_server=OFF
cmake --build "$out" --target QWaylandIntegrationPlugin QWaylandXdgShellIntegrationPlugin QWaylandEglClientBufferPlugin \
    QWaylandIntegrationPlugin_init QWaylandXdgShellIntegrationPlugin_init QWaylandEglClientBufferPlugin_init \
    QWaylandBradientDecorationPlugin QWaylandBradientDecorationPlugin_init --parallel "${JOBS:-8}"
"$g/cc" -Os -D_POSIX_C_SOURCE=200809L -c "$port/emutls.c" -o "$b/qt-emutls.o"
gui="$b/FreeCAD-4fd3bf320d9566a27e60069fc8387448aaa3a094/src/Gui"
inc=(-I"$gui" -I"$q/include" -I"$q/include/QtCore" -I"$q/include/QtGui" -I"$q/include/QtWidgets")
"$host/libexec/moc" "${inc[@]}" "$gui/SplitButton.h" -o "$b/moc_WaylandSplitButton.cpp"
"$g/cxx" -Os -std=c++20 -D_GNU_SOURCE -DTEST_WAYLAND "$b/qt-emutls.o" -Wl,--wrap=__emutls_get_address "${inc[@]}" "$port/test-qt.cpp" "$gui/SplitButton.cpp" \
    "$b/moc_WaylandSplitButton.cpp" -L"$q/lib" -L"$out/lib" -L"$b/sdk/lib" -L"$w/lib" \
    -L"$g/sysroot/lib" -L"$c/gtk3-port/sysroot/lib" -L"$c/dbus-port/sdk/lib" -Wl,--start-group \
    "$out/plugins/platforms/libqwayland-generic.a" "$out/plugins/wayland-shell-integration/libxdg-shell.a" \
    -lQt6WaylandClient -lQt6Widgets -lQt6Gui -lQt6DBus -lQt6Core -lQt6BundledPcre2 -lQt6BundledFreetype \
    -lQt6BundledLibpng -lharfbuzz -lxkbcommon -lpcre2-8 -lwayland-client -lwayland-cursor -lffi -lGL -lEGL \
    -ldrm -ldbus-1 -lz -Wl,--end-group -o "$b/test-wayland"
