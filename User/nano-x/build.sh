#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/nano-x"
revision=36c8d0edfa5423600027438b8f4bf0aa5bb49d28
digest=baf7690d91fbf163bf2d209266d82d71a848ce263850073bc496242f634b9057
archive="$build/upstream.tar.gz"
src="$build/microwindows-$revision/src"
musl="$cache/cpython-port"
gcc="${LIXINOS_GCC_BUILD:-$root/Kernel/target/gcc-port}"
if [[ ! -x "$musl/cc" ]]; then bash "$root/scripts/build-user.sh"; fi
if ! command -v aarch64-none-elf-objcopy >/dev/null; then
    cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
    export PATH="${cross[0]}/bin:$PATH"
fi
bash "$root/User/gcc/build.sh" rt
mkdir -p "$build"
if [[ ! -f "$archive" ]]; then
    curl -fL --retry 3 "https://codeload.github.com/ghaerr/microwindows/tar.gz/$revision" -o "$archive.tmp"
    mv "$archive.tmp" "$archive"
fi
printf '%s  %s\n' "$digest" "$archive" | shasum -a 256 -c -
if [[ ! -f "$src/include/nano-X.h" ]]; then tar -xzf "$archive" -C "$build"; fi

flags=(-Os -ffunction-sections -fdata-sections -DNONETWORK=1 -DNANOWM=1 -DNUKLEARUI=0
       -DUNIX=1 -DHAVE_SELECT=0 -DHAVE_SIGNAL=0 -DMW_FEATURE_TIMERS=0
       -DMW_FEATURE_IMAGES=0 -DMWPIXEL_FORMAT=MWPF_TRUECOLORARGB)
server=(srvmain srvfunc srvutil srvevent srvclip srvnonet nxdraw nxpaintnc nxutil
        wmaction wmclients wmevents wmutil)
drivers=(fb fblin1 fblin2 fblin4 fblin8 fblin16 fblin24 fblin32 fbportrait_left
         fbportrait_right fbportrait_down genmem genfont rasterops osdep kbd_null mou_null)
fonts=(rom8x16 rom8x8 winFreeSansSerif11x13 winFreeSystem14x16 X6x13)
engine=(devopen devdraw devblit convblit_8888 convblit_mask convblit_frameb devfont
        devmouse devkbd devclip devrgn devpal1 devpal2 devpal4 devpal8 devlist
        selfont devrgn2 devarc devpoly devstipple font_dbcs devimage devimage_stretch)
sources=()
for name in "${engine[@]}"; do sources+=("$src/engine/$name.c"); done
for name in "${server[@]}"; do sources+=("$src/nanox/$name.c"); done
for name in "${drivers[@]}"; do sources+=("$src/drivers/$name.c"); done
for name in "${fonts[@]}"; do sources+=("$src/fonts/compiled/$name.c"); done

aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$build/crt1.o"
"$musl/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I"$root/User/gcc" \
    -c "$root/User/cpython/loader.c" -o "$build/crt0.o"
package="$cache/nano-x-package"
mkdir -p "$package/bin" "$package/lib/nano-x"
"$musl/cc" "${flags[@]}" -I"$src/include" -I"$src/drivers" -I"$root/User" \
    -nostartfiles -static -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
    -Wl,-T,"$root/User/gcc/application.ld" "$build/crt0.o" "$build/crt1.o" \
    "$port/desktop.c" "$port/screen.c" "${sources[@]}" \
    -Wl,--start-group "$gcc/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group \
    -o "$build/nano-x"
aarch64-none-elf-strip -o "$package/bin/nano-x" "$build/nano-x"
cp "$src/../LICENSE" "$package/lib/nano-x/LICENSE"
echo "Nano-X package: $package"
