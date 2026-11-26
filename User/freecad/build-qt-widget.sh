#!/usr/bin/env bash
# Build the original FreeCAD SplitButton and a headless interaction test.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/freecad-port"
g="$cache/gnome-port"
gtk="$cache/gtk3-port"
src="$build/qtbase-everywhere-src-6.8.3"
q="$build/qt-build"
host="${QT_HOST_PATH:-$build/qt-host/6.8.3/macos}"
gui="$build/FreeCAD-4fd3bf320d9566a27e60069fc8387448aaa3a094/src/Gui"
[[ -f "$gui/SplitButton.cpp" ]] || bash "$port/build.sh"
if [[ ! -x "$host/libexec/moc" ]]; then
    printf 'Set QT_HOST_PATH to a host Qt 6.8.3 SDK containing libexec/moc.\n' >&2
    exit 1
fi
source "$port/fetch.sh"
fetch_archive "$build/qtbase.tar.xz" 56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80 https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz
[[ -d "$src" ]] || tar xf "$build/qtbase.tar.xz" -C "$build"
for fix in qt-thread-pipe.patch qt-semaphore.patch qt-static-egl.patch; do
    if ! patch -f -Rs --dry-run -p1 -d "$src" < "$port/$fix" >/dev/null; then
        patch -f -s -p1 -d "$src" < "$port/$fix"
    fi
done
cross=("$cache"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$gtk/tools/bin:$PATH"
export PKG_CONFIG_LIBDIR="$g/sysroot/lib/pkgconfig:$gtk/sysroot/lib/pkgconfig:$cache/cpython-port/sysroot/lib/pkgconfig"
opts=(-DINPUT_opengl=no -DCMAKE_INSTALL_PREFIX="$build/sdk")
targets=(Widgets QOffscreenIntegrationPlugin)
case "${1:-}" in
    --opengl)
        [[ -f "$build/sdk/lib/libGL.a" ]] || bash "$port/build-gl.sh"
        q="$build/qt-gl-build"
        opts=(-DINPUT_opengl=desktop -DFEATURE_mtdev=OFF -DFEATURE_xlib=OFF -DCMAKE_INSTALL_PREFIX="$build/qt-sdk"
              -DOPENGL_gl_LIBRARY="$build/sdk/lib/libGL.a" -DOPENGL_INCLUDE_DIR="$build/sdk/include"
              -DCMAKE_C_FLAGS="-D_GNU_SOURCE -I$g/sysroot/include" -DCMAKE_CXX_FLAGS="-D_GNU_SOURCE -I$g/sysroot/include")
        opts+=(-UHAVE_EGL "-DLIXINOS_EXTRA_ROOTS=$cache/wayland-port/sdk" -DFEATURE_wayland=ON
              -DFEATURE_egl=ON -DFEATURE_eglfs=OFF
              "-DEGL_STATIC_DEPENDENCIES=$g/sysroot/lib/libdrm.a;$cache/cpython-port/sysroot/lib/libz.a")
        targets=(OpenGLWidgets Network Concurrent Xml PrintSupport PrintSupport_resources_1 PrintSupport_resources_2
                 QGifPlugin QGifPlugin_init QICOPlugin QICOPlugin_init QJpegPlugin QJpegPlugin_init
                 QNetworkManagerNetworkInformationPlugin QNetworkManagerNetworkInformationPlugin_init
                 QTlsBackendCertOnlyPlugin QTlsBackendCertOnlyPlugin_init)
        ;;
    "") ;;
    *) echo "usage: $0 [--opengl]" >&2; exit 2 ;;
esac
cmake -S "$src" -B "$q" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$port/lixinos.cmake" \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DQT_HOST_PATH="$host" \
    -DQT_QMAKE_TARGET_MKSPEC=linux-g++ -DBUILD_SHARED_LIBS=OFF "${opts[@]}" \
    -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF -DFEATURE_xcb=OFF -DFEATURE_glib=OFF -DFEATURE_fontconfig=OFF
cmake --build "$q" --target "${targets[@]}" --parallel "${JOBS:-8}"
[[ ${1:-} != --opengl ]] || exit 0
inc=(-I"$gui" -I"$q/include" -I"$q/include/QtCore" -I"$q/include/QtGui" -I"$q/include/QtWidgets")
"$host/libexec/moc" "${inc[@]}" "$gui/SplitButton.h" -o "$build/moc_SplitButton.cpp"
"$g/cxx" -Os -std=c++20 -D_GNU_SOURCE "${inc[@]}" "$port/test-qt.cpp" "$gui/SplitButton.cpp" \
    "$build/moc_SplitButton.cpp" -L"$g/sysroot/lib" -L"$gtk/sysroot/lib" -L"$q/lib" \
    -Wl,--start-group "$q/plugins/platforms/libqoffscreen.a" -lQt6Widgets -lQt6Gui -lQt6Core \
    -lQt6BundledPcre2 -lQt6BundledFreetype -lQt6BundledLibpng -lharfbuzz -lz -lxkbcommon -lpcre2-8 \
    -Wl,--end-group -o "$build/test-qt"
