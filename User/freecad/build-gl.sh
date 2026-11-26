#!/usr/bin/env bash
# Install desktop GL entry points and GLU into the FreeCAD target sysroot.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
g="$cache/gnome-port"
build="$cache/freecad-port"
sdk="$build/sdk"
mesa="$g/mesa-24.3.4"
[[ -f "$g/sysroot/lib/libEGL.a" ]] || bash "$root/User/gnome/build-mesa.sh"
cross=("$cache"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$g/mesa-tools/bin:$PATH"
mkdir -p "$sdk/lib/pkgconfig" "$sdk/include/GL"
ninja -C "$g/mesa-egl" src/mapi/glapi/libglapi_bridge.a
(
    cd "$g/mesa-egl"
    printf 'create libdesktopGL.a\naddlib src/mapi/glapi/libglapi_bridge.a\nsave\nend\n' | aarch64-none-elf-ar -M
    install -m 644 libdesktopGL.a "$sdk/lib/libGL.a"
)
install -m 644 "$mesa"/include/GL/{gl.h,glext.h,glcorearb.h} "$sdk/include/GL/"
cat > "$sdk/lib/pkgconfig/gl.pc" <<PC
prefix=\${pcfiledir}/../..
Name: gl
Description: LixinOS desktop OpenGL via Mesa EGL
Version: 24.3.4
Requires.private: egl
Libs: -L\${prefix}/lib -lGL
Cflags: -I\${prefix}/include
PC
src="$build/glu-9.0.3"
source "$port/fetch.sh"
fetch_archive "$build/glu.tar.xz" bd43fe12f374b1192eb15fe20e45ff456b9bc26ab57f0eee919f96ca0f8a330f https://archive.mesa3d.org/glu/glu-9.0.3.tar.xz
[[ -d "$src" ]] || tar xf "$build/glu.tar.xz" -C "$build"
cat > "$build/glu-cross.ini" <<INI
[properties]
pkg_config_libdir = '$sdk/lib/pkgconfig:$g/sysroot/lib/pkgconfig:$cache/gtk3-port/sysroot/lib/pkgconfig:$cache/cpython-port/sysroot/lib/pkgconfig'
INI
meson setup "$build/glu-build" "$src" --cross-file "$g/cross.ini" --cross-file "$build/glu-cross.ini" \
    --prefix "$sdk" --libdir lib --buildtype minsize -Ddefault_library=static -Dgl_provider=gl
meson install -C "$build/glu-build"
"$g/cxx" -Os "$port/test-gl.cpp" -I"$sdk/include" -I"$g/sysroot/include" \
    -L"$sdk/lib" -L"$g/sysroot/lib" -Wl,--start-group -lGLU -lGL -lEGL -ldrm -lz -Wl,--end-group -o "$build/test-gl"
