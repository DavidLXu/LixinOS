#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gnome-port"
prefix="$build/sysroot"
wayland="$cache/wayland-port/sdk"
musl="$cache/cpython-port/sysroot"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$wayland/bin:$cache/gtk3-port/tools/bin:${cross[0]}/bin:$PATH"
export PKG_CONFIG_PATH="$wayland/lib/pkgconfig"
[[ -d "$build/gtk-4.20.0" && -f "$prefix/lib/libpango-1.0.a" ]] || bash "$port/build-deps.sh"
[[ -f "$prefix/lib/libEGL.a" ]] || bash "$port/build-mesa.sh"
[[ -f "$prefix/include/epoxy/egl.h" ]] || bash "$port/build-epoxy.sh"
jobs=${JOBS:-8}

if [[ ! -f "$prefix/lib/libtiff.a" ]]; then
    cmake -S "$build/tiff-4.7.0" -B "$build/tiff-cross" -G Ninja \
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_C_COMPILER="$build/cc" -DCMAKE_CXX_COMPILER="$build/cxx" \
        -DCMAKE_AR="$(command -v aarch64-none-elf-ar)" -DCMAKE_RANLIB="$(command -v aarch64-none-elf-ranlib)" \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DBUILD_SHARED_LIBS=OFF -Dtiff-tools=OFF -Dtiff-tests=OFF -Dtiff-contrib=OFF -Dtiff-docs=OFF \
        -Dtiff-opengl=OFF -Dcxx=OFF -Djpeg=OFF -Djbig=OFF -Dlzma=OFF -Dwebp=OFF -Dlerc=OFF \
        -Dzstd=OFF -Dlibdeflate=OFF -Dzlib=ON -DZLIB_INCLUDE_DIR="$musl/include" \
        -DZLIB_LIBRARY="$musl/lib/libz.a"
    cmake --build "$build/tiff-cross" --parallel "$jobs"
    cmake --install "$build/tiff-cross"
fi

if [[ ! -f "$prefix/lib/libcroco-0.6.a" ]]; then
    mkdir -p "$build/croco-cross"
    (
        cd "$build/croco-cross"
        export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$cache/gtk3-port/sysroot/lib/pkgconfig"
        CC="$build/cc" AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib NM=aarch64-none-elf-nm \
            "$build/libcroco-0.6.13/configure" --host=aarch64-linux-musl --prefix="$prefix" \
            --libdir="$prefix/lib" --disable-shared --enable-static --disable-gtk-doc --disable-Bsymbolic
        make -C src -j "$jobs" install
    )
fi
install -m 644 "$build/croco-cross/libcroco.pc" "$prefix/lib/pkgconfig/libcroco-0.6.pc"

src="$build/gtk-4.20.0"
stage="$build/gtk4-cross"
ln -sfn "$build/librsvg-librsvg-gtk-2.40.23" "$src/subprojects/librsvg"
if ! patch -f -Rs --dry-run -p1 -d "$src" < "$port/gtk4-static.patch" >/dev/null 2>&1; then
    patch -f -s -p1 -d "$src" < "$port/gtk4-static.patch"
fi
options=(-Dprefer_static=true -Dx11-backend=false -Dwayland-backend=true -Dintrospection=disabled
         -Dvulkan=disabled -Dmedia-gstreamer=disabled -Dprint-cups=disabled
         -Dbuild-demos=false -Dbuild-examples=false -Dbuild-tests=false -Dbuild-testsuite=false
         -Dlibrsvg:tests=disabled -Dlibrsvg:rsvg-convert=disabled -Dlibrsvg:rsvg-view-3=disabled)
if [[ -f "$stage/build.ninja" ]]; then
    meson setup --reconfigure "$stage" "$src" "${options[@]}"
else
    meson setup "$stage" "$src" --cross-file "$build/cross.ini" --prefix "$prefix" --libdir lib \
        --default-library static --buildtype minsize --wrap-mode nodownload "${options[@]}"
fi
meson compile -C "$stage" -j "$jobs" gtk-4 rsvg-2
meson install --no-rebuild -C "$stage" --tags devel
