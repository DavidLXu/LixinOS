#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../../.." && pwd)
cache="$root/Kernel/target/user-build"
mj="$cache/mujoco-port"
gnome="$cache/gnome-port"
export PORT="$port" SRC="$mj/mujoco-4.0.1" WORK="$mj/viewer" GNOME="$gnome"
export DEPS="$WORK/deps"
[[ -f "$mj/libmujoco-engine.a" ]] || bash "$port/../build.sh"
mkdir -p "$DEPS" "$WORK/obj"
fetch() {
    local name=$1 repo=$2 rev=$3 digest=$4
    if [[ ! -d "$DEPS/$name-$rev" ]]; then
        curl -fLsS --retry 3 "https://codeload.github.com/$repo/tar.gz/$rev" -o "$DEPS/$name.tar.gz"
        printf '%s  %s\n' "$digest" "$DEPS/$name.tar.gz" | shasum -a 256 -c -
        tar xf "$DEPS/$name.tar.gz" -C "$DEPS"
    fi
    ln -sfn "$name-$rev" "$DEPS/$name"
}
fetch tinyxml2 leethomason/tinyxml2 e6caeae85799003f4ca74ff26ee16a789bc2af48 ab1a6700074ab4d468e46535545bb33aa4a74d794ab514fac64cc297fc7a2545
fetch tinyobjloader tinyobjloader/tinyobjloader 2945a967c5303b2c8c14174117c45f3302591150 988cc315f69c9319499fcb20e4feb54e058ea99d1b69d18e9a05984ce884f49c
fetch MarchingCubeCpp aparis69/MarchingCubeCpp f03a1b3ec29b1d7d865691ca8aea4f1eb2c2873d 227c10b2cffe886454b92a0e9ef9f0c9e8e001d00ea156cc37c8fc43055c9ca6
fetch qhull qhull/qhull d1c2fc0caa5f644f3a0f220290d4a868c68ed4f6 421177cc21a7dcb4c1bbc51f65bc16f21d1f157814116bb5c341d694e23d154d
fetch miniz richgel999/miniz 77d0dce8627735138c51770d1799a1ef48f2117d baa14349e7c5efb4a493a03af7d2f29d0ef45d87d488567fe5360edb33f8b7ba
fetch lodepng lvandeve/lodepng 22561883dd63fd1850f18e1f6adac321e4f609b0 f18bb3b6fb7e0cf8500fd84a46d9047b609a346be2e21609d20893d547182830
[[ -f "$WORK/miniz_export.h" ]] || printf '#define MINIZ_EXPORT\n' > "$WORK/miniz_export.h"
if [[ ! -f "$WORK/patched.stamp" || "$port/win3.patch" -nt "$WORK/patched.stamp" ]]; then
    mkdir -p "$WORK/adapt/simulate" "$WORK/adapt/src/engine"
    cp "$SRC/simulate/"{main,simulate}.cc "$WORK/adapt/simulate/"
    mkdir -p "$WORK/adapt/src/render/classic"
    cp "$SRC/src/render/classic/render_context.c" "$WORK/adapt/src/render/classic/"
    mkdir -p "$WORK/adapt/src/xml"
    cp "$SRC/src/xml/xml_global.cc" "$WORK/adapt/src/xml/"
    cp "$SRC/src/engine/"{engine_thread.cc,engine_util_errmem.c} "$WORK/adapt/src/engine/"
    patch -s -d "$WORK/adapt" -p1 < "$port/win3.patch"
    touch "$WORK/patched.stamp"
fi
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
mesa="$gnome/sysroot"
[[ -f "$mesa/lib/libEGL.a" ]] || bash "$root/User/gnome/build-mesa.sh"
export PKG_CONFIG_LIBDIR="$mesa/lib/pkgconfig:$cache/gtk3-port/sysroot/lib/pkgconfig"
export MESA_FLAGS="$(pkg-config --static --cflags --libs glesv2)"
LIXINOS_GCC_BUILD="$gnome/runtime" bash "$root/User/gcc/build.sh" rt
export ENGINE="$mj/libmujoco-engine.a"
make -f "$port/Makefile" -j "${JOBS:-10}" "$WORK/simulate"
python3 "$port/package.py" "$root" "$WORK" "$cache/simulate-package"
