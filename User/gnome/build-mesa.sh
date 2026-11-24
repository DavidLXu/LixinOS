#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gnome-port"
prefix="$build/sysroot"
stage="$build/mesa-egl"
src="$build/mesa-24.3.4"

if [[ ! -f "$prefix/lib/libdrm.a" || ! -x "$build/cxx" ]]; then
    bash "$port/build-deps.sh"
fi
tools="$build/mesa-tools"
if [[ ! -x "$tools/bin/meson" ]] || ! "$tools/bin/python3" -c 'import mako, packaging, yaml'; then
    python3 -m venv "$tools"
    "$tools/bin/pip" install meson==1.7.2 ninja==1.11.1.4 Mako==1.3.10 packaging==25.0 PyYAML==6.0.2
fi
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$tools/bin:/opt/homebrew/opt/bison/bin:${cross[0]}/bin:$PATH"
for diff in "$port/mesa-static.patch" "$port/mesa-gbm.patch" "$port/mesa-virgl.patch"; do
    if ! patch -f -Rs --dry-run -p1 -d "$src" < "$diff" >/dev/null 2>&1; then
        patch -f -s -p1 -d "$src" < "$diff"
    fi
done
install -m 644 "$port/egl-check.c" "$src/src/egl/lixin-egl-check.c"
vtest="$src/src/gallium/winsys/virgl/vtest"
install -m 644 "$port/virgl-transport.h" "$root/Kernel/native/gpu-abi.h" "$vtest"
options=(-Dplatforms=[] -Dgallium-drivers=softpipe,virgl -Dvulkan-drivers=[] -Dglx=disabled -Degl=enabled
         -Degl-native-platform=surfaceless -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled -Dllvm=disabled
         -Dshared-glapi=enabled -Dglvnd=disabled -Dlibunwind=disabled -Dxmlconfig=disabled -Dexpat=disabled
         -Dvideo-codecs=[] -Dshader-cache=disabled -Dbuild-tests=false
         "-Dc_args=['-ffunction-sections','-fdata-sections','-I$root/User/gui']")
if [[ -f "$stage/build.ninja" ]]; then
    meson setup --reconfigure "$stage" "$src" "${options[@]}"
else
    meson setup "$stage" "$src" --cross-file "$build/cross.ini" --prefix "$prefix" --libdir lib \
        --buildtype minsize --wrap-mode nodownload "${options[@]}"
fi
ninja -C "$stage" -j "${JOBS:-8}" src/egl/lixin-egl-check
python3 "$port/install-mesa-sdk.py" "$stage" "$src" "$prefix"
