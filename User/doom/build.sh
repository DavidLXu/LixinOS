#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/doom-port"
revision=e257a09527062bfb986b6e5e0780839b6cbd5b71
freedoom=0.12.1
src="$build/doomgeneric-$revision"
package="$build/package"
mkdir -p "$build/obj" "$package/bin" "$package/lib/doom"

download() {
    local name=$1 hash=$2 url=$3
    if [[ ! -f "$build/$name" ]]; then
        curl -fLsS --retry 3 "$url" -o "$build/$name.tmp"
        printf '%s  %s\n' "$hash" "$build/$name.tmp" | shasum -a 256 -c -
        mv "$build/$name.tmp" "$build/$name"
    fi
    printf '%s  %s\n' "$hash" "$build/$name" | shasum -a 256 -c -
}
download engine.tar.gz 26f8f60daa6c6e4d3c01e5eac9fb700d592dede803d13ac848154ff356b2a2e7 \
    "https://codeload.github.com/ozkl/doomgeneric/tar.gz/$revision"
download freedoom.zip f42c6810fc89b0282de1466c2c9c7c9818031a8d556256a6db1b69f6a77b5806 \
    "https://github.com/freedoom/freedoom/releases/download/v$freedoom/freedoom-$freedoom.zip"
tar -xf "$build/engine.tar.gz" -C "$build"

musl="$cache/cpython-port"
LIXINOS_GCC_BUILD="$build/runtime" bash "$root/User/gcc/build.sh" rt
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$build/obj/crt1.o"
"$musl/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I"$root/User/gcc" \
    -c "$root/User/cpython/loader.c" -o "$build/obj/crt0.o"

engine="$src/doomgeneric"
sed -i.bak 's/M_SetConfigDir(NULL);/M_SetConfigDir(".doom\/");/' "$engine/d_main.c"
# The opening melt reads the previous frame before any scene was drawn.
sed -i.bak '/I_VideoBuffer = (byte\*)Z_Malloc /a\
    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT);\
' "$engine/i_video.c"
objects=()
read -r -a sources <<< "$(make -s -C "$engine" print OBJDIR=. | sed 's/^OBJS: //')"
flags=(-O2 -std=gnu11 -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -fcommon -ffunction-sections -fdata-sections
       -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -I"$engine")
for name in "${sources[@]}"; do
    name=${name#./}
    [[ "$name" == doomgeneric_xlib.o ]] && continue
    objects+=("$build/obj/$name")
    "$musl/cc" "${flags[@]}" -c "$engine/${name%.o}.c" -o "$build/obj/$name"
done
"$musl/cc" "${flags[@]}" -c "$port/platform.c" -o "$build/obj/platform.o"
"$musl/cc" -nostartfiles -static -s -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
    -Wl,-T,"$root/User/gcc/application.ld" "$build/obj/crt0.o" "$build/obj/crt1.o" \
    "${objects[@]}" "$build/obj/platform.o" \
    -Wl,--start-group "$build/runtime/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group -o "$package/bin/doom"

cp "$src/LICENSE" "$package/lib/doom/DOOM-ENGINE-LICENSE"
for name in freedoom2.wad COPYING.txt CREDITS.txt; do
    unzip -p "$build/freedoom.zip" "freedoom-$freedoom/$name" > "$package/lib/doom/$name"
done
echo "$package"
