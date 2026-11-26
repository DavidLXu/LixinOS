#!/usr/bin/env bash
# Build Mesa's Wayland wl_shm software EGL driver for Qt's onscreen 3D view.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
g="$cache/gnome-port"
b="$cache/freecad-port"
src="$g/mesa-24.3.4"
stage="$b/mesa-wayland"
w="$cache/wayland-port/sdk"
[[ -f "$g/mesa-egl/build.ninja" ]] || bash "$root/User/gnome/build-mesa.sh"
cross=("$cache"/arm-gnu-toolchain-*/bin)
export PATH="$g/mesa-tools/bin:$w/bin:/opt/homebrew/opt/bison/bin:${cross[0]}:$PATH"
export PKG_CONFIG_PATH_FOR_BUILD="$w/lib/pkgconfig"
export PKG_CONFIG_PATH="$w/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if ! patch -f -Rs --dry-run -p1 -d "$src" < "$port/mesa-wayland-api.patch" >/dev/null; then
    patch -f -s -p1 -d "$src" < "$port/mesa-wayland-api.patch"
fi
meson setup --reconfigure "$stage" "$src" --cross-file "$g/cross.ini" --prefix "$b/wayland-egl-sdk" --libdir lib \
    --buildtype minsize --wrap-mode nodownload -Dprefer_static=true -Dplatforms=wayland -Dgallium-drivers=softpipe,virgl \
    -Dvulkan-drivers=[] -Dglx=disabled -Degl=enabled -Degl-native-platform=wayland -Dgbm=enabled \
    -Dgles1=disabled -Dgles2=enabled -Dllvm=disabled -Dshared-glapi=enabled -Dglvnd=disabled \
    -Dlibunwind=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dvideo-codecs=[] -Dshader-cache=disabled \
    -Dbuild-tests=false "-Dc_args=['-ffunction-sections','-fdata-sections','-I$root/User/gui']"
python3 "$port/bundle-wayland-egl.py" "$stage" "$b/wayland-egl-sdk/lib/libEGL.a"

q="$b/qt-gl-build"
wlqt="$b/qt-wayland-build"
"$g/cc" -c "$port/once-tls.S" -o "$stage/once-tls.o"
"$g/cc" -Os -DLIBSTDCXX_ONCE_TLS -D_POSIX_C_SOURCE=200809L -c "$port/emutls.c" -o "$stage/emutls.o"
"$g/cxx" -Os -std=c++20 -D_GNU_SOURCE "$port/test-qt-view.cpp" "$port/test-coin.cpp" \
    "$stage/emutls.o" "$stage/once-tls.o" -Wl,--wrap=__emutls_get_address -I"$q/include" -I"$q/include/QtCore" \
    -I"$q/include/QtGui" -I"$q/include/QtWidgets" -I"$q/include/QtOpenGLWidgets" -I"$b/sdk/include" -I"$g/sysroot/include" \
    -L"$b/wayland-egl-sdk/lib" -L"$q/lib" -L"$wlqt/lib" -L"$b/sdk/lib" -L"$w/lib" \
    -L"$g/sysroot/lib" -L"$cache/gtk3-port/sysroot/lib" -L"$cache/dbus-port/sdk/lib" -Wl,--start-group \
    "$wlqt/plugins/platforms/libqwayland-generic.a" "$wlqt/plugins/wayland-shell-integration/libxdg-shell.a" \
    "$wlqt/plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.a" \
    -lQt6WaylandEglClientHwIntegration -lQt6OpenGLWidgets -lQt6OpenGL -lQt6WaylandClient -lQt6Widgets -lQt6Gui -lQt6DBus -lQt6Core \
    -lQt6BundledPcre2 -lQt6BundledFreetype -lQt6BundledLibpng -lharfbuzz -lxkbcommon -lpcre2-8 \
    -lwayland-client -lwayland-server -lwayland-cursor -lwayland-egl -lffi -lCoin -lGLU -lGL -lEGL \
    -ldrm -ldbus-1 -lz -Wl,--end-group -o "$b/test-view"
