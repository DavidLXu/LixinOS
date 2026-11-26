#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/netsurf-port"
src="$build/netsurf-all-3.11"
prefix="$build/sysroot"
musl="$cache/cpython-port"
if [[ ! -x "$musl/cc" || ! -f "$cache/desktop-font.o" || ! -f "$cache/links-port/anchors.h" ||
      ! -f "$cache/bearssl-0.6/arm/libbearssl.a" ]]; then
    bash "$root/scripts/build-user.sh"
fi
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
mkdir -p "$build" "$prefix"
if [[ ! -f "$build/source.tar.gz" ]]; then
    curl -fL --retry 3 https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz -o "$build/source.tmp"
    mv "$build/source.tmp" "$build/source.tar.gz"
fi
echo "4dea880ff3c2f698bfd62c982b259340f9abcd7f67e6c8eb2b32c61f71644b7b  $build/source.tar.gz" | shasum -a 256 -c -
if [[ ! -f "$src/netsurf/Makefile" ]]; then tar -xzf "$build/source.tar.gz" -C "$build"; fi
if [[ ! -f "$build/stb_truetype.h" ]]; then
    curl -fL --retry 3 https://raw.githubusercontent.com/nothings/stb/f0569113c93ad095470c54bf34a17b36646bbbb5/stb_truetype.h -o "$build/stb.tmp"
    mv "$build/stb.tmp" "$build/stb_truetype.h"
fi
echo "ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab  $build/stb_truetype.h" | shasum -a 256 -c -
host_png_cflags=$(pkg-config --cflags libpng)
host_png_libs=$(pkg-config --libs libpng)
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig" PKG_CONFIG_PATH=""
export CFLAGS="-Os -ffunction-sections -fdata-sections -Wno-error"
libargs=(HOST=aarch64-lixinos CC="$musl/cc" AR=aarch64-none-elf-ar PREFIX="$prefix"
         NSSHARED="$src/buildsystem" WARNFLAGS="-Wall -Wno-error" PKGCONFIG=pkg-config
         WITH_EXPAT_BINDING=no INSTALL=install)
# Patch a pristine tree. Separate builds can share the downloaded archive.
apply_source_patch() {
    local diff=$1 staging
    staging=$(mktemp -d "$build/patch.XXXXXXXX")
    sed -n 's|^--- a/|netsurf-all-3.11/|p' "$diff" > "$staging/files"
    tar -xzf "$build/source.tar.gz" -C "$staging" -T "$staging/files"
    patch -f -p1 -d "$staging/netsurf-all-3.11" < "$diff"
    while IFS= read -r file; do
        mv -f "$staging/$file" "$build/$file"
    done < "$staging/files"
    rm -r "$staging"
}
hubbub_id=$(shasum -a 256 "$port/hubbub.patch")
if [[ ! -f "$build/hubbub.id" || $(cat "$build/hubbub.id") != "$hubbub_id" ]]; then
    apply_source_patch "$port/hubbub.patch"
    echo "$hubbub_id" > "$build/hubbub.id"
fi
dom_id=$(shasum -a 256 "$port/libdom.patch")
if [[ ! -f "$build/libdom.id" || $(cat "$build/libdom.id") != "$dom_id" ]]; then
    apply_source_patch "$port/libdom.patch"
    echo "$dom_id" > "$build/libdom.id"
fi
css_id=$(shasum -a 256 "$port/libcss.patch" "$port/grid_parse.inc")
if [[ ! -f "$build/libcss.id" || $(cat "$build/libcss.id") != "$css_id" ]]; then
    apply_source_patch "$port/libcss.patch"
    cp "$port/grid_parse.inc" "$src/libcss/src/parse/properties/"
    (cd "$src/libcss" && python3 src/select/select_generator.py)
    echo "$css_id" > "$build/libcss.id"
fi
for lib in libwapcaplet libparserutils libhubbub libcss libdom libnsutils libnsgif libnsbmp libnsfb; do
    if [[ "$lib" == libdom || "$lib" == libcss ]]; then
        CFLAGS="$CFLAGS -Oz -flto=auto" make -s -j6 -C "$src/$lib" "${libargs[@]}" \
            BUILDDIR=build-lixinos-lto AR=aarch64-none-elf-gcc-ar install
    else
        make -s -j6 -C "$src/$lib" "${libargs[@]}" install
    fi
done
# DOM bindings are generated on the build host, then compiled for LixinOS.
if command -v brew >/dev/null && [[ -x "$(brew --prefix bison)/bin/bison" ]]; then
    export PATH="$(brew --prefix bison)/bin:$PATH"
fi
gen_id=$(shasum -a 256 "$port/nsgenbind.patch")
if [[ ! -f "$build/nsgenbind.id" || $(cat "$build/nsgenbind.id") != "$gen_id" ]]; then
    apply_source_patch "$port/nsgenbind.patch"
    rm -f "$src/netsurf/build/LixinOS-framebuffer/duktape/Makefile"
    echo "$gen_id" > "$build/nsgenbind.id"
fi
make -s -C "$src/nsgenbind" NSSHARED="$src/buildsystem" PREFIX="$build/host" WARNFLAGS=-Wall INSTALL=install install
export PATH="$build/host/bin:$PATH"
source "$port/svg-build.sh"
source "$port/shaping-build.sh"
png="$build/libpng-1.6.48"
if [[ ! -f "$build/libpng.tar.gz" ]]; then
    curl -fL --retry 3 https://codeload.github.com/pnggroup/libpng/tar.gz/refs/tags/v1.6.48 -o "$build/png.tmp"
    mv "$build/png.tmp" "$build/libpng.tar.gz"
fi
echo "b17e99026055727e8cba99160c3a9a7f9af788e9f786daeadded5a42243f1dd0  $build/libpng.tar.gz" | shasum -a 256 -c -
if [[ ! -f "$prefix/lib/libpng.a" ]]; then
    tar -xzf "$build/libpng.tar.gz" -C "$build"
    (cd "$png"
     cp scripts/pnglibconf.h.prebuilt pnglibconf.h
     "$musl/cc" -Os -DPNG_ARM_NEON_OPT=0 -I. -ffunction-sections -fdata-sections -c png{,error,get,mem,pread,read,rio,rtran,rutil,set,trans,wio,write,wtran,wutil}.c
     aarch64-none-elf-ar rcs "$prefix/lib/libpng.tmp" png{,error,get,mem,pread,read,rio,rtran,rutil,set,trans,wio,write,wtran,wutil}.o
     cp png.h pngconf.h pnglibconf.h "$prefix/include/"
     mv "$prefix/lib/libpng.tmp" "$prefix/lib/libpng.a")
fi
jpeg="$build/jpeg-9f"
if [[ ! -f "$build/jpeg.tar.gz" ]]; then
    curl -fL --retry 3 https://ijg.org/files/jpegsrc.v9f.tar.gz -o "$build/jpeg.tmp"
    mv "$build/jpeg.tmp" "$build/jpeg.tar.gz"
fi
echo "04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b  $build/jpeg.tar.gz" | shasum -a 256 -c -
if [[ ! -f "$prefix/lib/libjpeg.a" ]]; then
    tar -xzf "$build/jpeg.tar.gz" -C "$build"
    (cd "$jpeg"
     cp jconfig.txt jconfig.h
     make -s -j6 -f makefile.ansi CC="$musl/cc" CFLAGS="$CFLAGS" \
         AR="aarch64-none-elf-ar rc" AR2=aarch64-none-elf-ranlib libjpeg.a
     cp jpeglib.h jconfig.h jerror.h jmorecfg.h "$prefix/include/"
     cp libjpeg.a "$prefix/lib/libjpeg.a")
fi
cat > "$prefix/lib/pkgconfig/libpng.pc" <<EOF
Name: libpng
Description: PNG decoder
Version: 1.6.48
Libs: -L$prefix/lib -lpng -lz
Cflags: -I$prefix/include
EOF
rt="${LIXINOS_GCC_BUILD:-$root/Kernel/target/gcc-port}/rt"
bash "$root/User/gcc/build.sh" rt
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$build/crt1.o"
"$musl/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -D_start=lx_app_start -I"$root/User/gcc" -c "$root/User/cpython/loader.c" -o "$build/crt0.o"
# Keep the desktop in the small image so terminal sessions can resume it.
sed -e 's|\*(.rodata\*) \*(.eh_frame\*)||' -e 's|\*(.data\*)|*(.rodata*) *(.eh_frame*) *(.data*)|' \
    "$root/User/gcc/application.ld" > "$build/browser.ld"
cat > "$build/Makefile.config" <<EOF
override NETSURF_LOG_LEVEL := WARNING
override NETSURF_USE_CURL := NO
override NETSURF_USE_OPENSSL := NO
override NETSURF_USE_JPEG := YES
override NETSURF_USE_DUKTAPE := YES
override NETSURF_USE_UTF8PROC := NO
override NETSURF_USE_PNG := YES
override NETSURF_FB_FRONTEND := lixin
override NETSURF_FB_RESPATH := /lib/netsurf
override NETSURF_HOMEPAGE := file:///lib/netsurf/welcome.html
CFLAGS += -I$port -D_GNU_SOURCE -Oz -flto=auto -ffunction-sections -fdata-sections
LDFLAGS += -flto=auto -Oz -static -nostartfiles -Wl,-Map,$build/browser.map,--gc-sections,-z,max-page-size=4096,--build-id=none,-T,$build/browser.ld $build/start.o $build/crt0.o $build/crt1.o -Wl,--start-group
EOF
cmp -s "$build/Makefile.config" "$src/netsurf/Makefile.config" || cp "$build/Makefile.config" "$src/netsurf/Makefile.config"
patch_id=$(shasum -a 256 "$port/lixinos.patch" | cut -d' ' -f1)
if [[ ! -f "$build/patch.id" || $(cat "$build/patch.id") != "$patch_id" ]]; then
    apply_source_patch "$port/lixinos.patch"
    echo "$patch_id" > "$build/patch.id"
fi
obj="$src/netsurf/build/LixinOS-framebuffer"
inc=(-I"$hb/src" -I"$build" -I"$port" -I"$root/User" -I"$src/netsurf" -I"$src/netsurf/include" -I"$prefix/include" -I"$obj"
     -I"$cache/links-port" -I"$cache/bearssl-0.6/inc")
"$musl/cc" -c "$port/start.S" -o "$build/start.o"
sources=("$root/User/gui/han-font.c" "$port/http.c" "$port/desktop-image.c" "$port/han.c" "$port/scalable.c" "$port/svg.c" "$port/root_lengths.c" "$root/User/desktop/desktop.c" "$root/User/links/load.c"
         "$root/User/links/tls.c" "$root/User/links/cookies.c" "$root/User/tcc/api.c")
objects=()
for file in "${sources[@]}"; do
    object="$build/$(basename "${file%.c}").o"
    "$musl/cc" -Oz -flto=auto -D_GNU_SOURCE -DLX_NETSURF -std=gnu11 -ffunction-sections -fdata-sections "${inc[@]}" -c "$file" -o "$object"
    objects+=("$object")
done
make SHELL=/bin/bash -j8 -C "$src/netsurf" -f Makefile -f "$port/target.mk" GBFLAGS= TARGET=framebuffer HOST=LixinOS CC="$musl/cc" \
    BUILD_LIBPNG_CFLAGS="$host_png_cflags" BUILD_LIBPNG_LDFLAGS="$host_png_libs" \
    PORT_OBJECTS="${objects[*]}" \
    PORT_LIBS="$hb/guest.o $prefix/lib/libplutosvg.a $prefix/lib/libplutovg.a $cache/bearssl-0.6/arm/libbearssl.a $rt/liblx.a -lc -lm -lgcc"
package="$build/package"
res="$src/netsurf/frontends/framebuffer/res"
mkdir -p "$package/lib/netsurf/fonts" "$package/bin" "$package/lib/netsurf/licenses"
# Extract the same little-endian table embedded by the smaller standalone desktop.
aarch64-none-elf-gcc -O0 -fdata-sections -x c -c "$root/User/desktop/launcher-pinyin.h" -o "$build/pinyin.o"
aarch64-none-elf-objcopy --dump-section ".rodata.launcher_dictionary=$package/lib/netsurf/launcher-pinyin" "$build/pinyin.o"
cp "$build"/NotoSans{,Arabic,Hebrew,Devanagari,Tamil}.ttf "$package/lib/netsurf/fonts/"
python3 "$root/User/desktop/launcher-icons.py" "$package/lib/netsurf/launcher-icons.bin"
cp "$root/User/desktop/icons/COPYING" "$package/lib/netsurf/licenses/Papirus"
cp "$root/User/desktop/icons/FLUENT-LICENSE" "$package/lib/netsurf/licenses/Fluent-Emoji"
cp "$hb/COPYING" "$package/lib/netsurf/licenses/HarfBuzz"
cp "$build/noto-OFL.txt" "$package/lib/netsurf/licenses/Noto"
swift "$root/User/desktop/outlines.swift" "$package/lib/netsurf/outlines"
swift "$root/User/desktop/latin.swift" "$package/lib/netsurf/latin"
swift "$root/User/desktop/pingfang.swift" "$package/lib/netsurf/pingfang"
aarch64-none-elf-strip -o "$package/bin/netsurf" "$src/netsurf/nsfb"
cp "$package/bin/netsurf" "$package/bin/desktop"
for file in Messages default.css quirks.css internal.css adblock.css favicon.png netsurf.png licence.html; do
    cp -L "$res/$file" "$package/lib/netsurf/$file"
done
cp "$port/welcome.html" "$port/welcome.css" "$port/js-demo.html" "$port/expand.js" "$package/lib/netsurf/"
for component in netsurf libwapcaplet libparserutils libhubbub libcss libdom libnsutils libnsgif libnsbmp libnsfb; do
    cp "$src/$component/COPYING" "$package/lib/netsurf/licenses/$component"
done
awk '/^\/\* LICENSE.txt \*\// { license=1; next } license && /^\*\// { exit }
     license && /^\*/ { sub(/^\* *( *)?/,""); print }' \
    "$src/netsurf/content/handlers/javascript/duktape/duktape.c" > "$package/lib/netsurf/licenses/Duktape"
sed -n '/^ALTERNATIVE A - MIT License/,$p' "$build/stb_truetype.h" > "$package/lib/netsurf/licenses/stb-truetype"
cp "$build/plutosvg-$svg_revision/LICENSE" "$package/lib/netsurf/licenses/plutosvg"
cp "$build/plutovg-$vg_revision/LICENSE" "$package/lib/netsurf/licenses/plutovg"
cp "$build/plutovg-$vg_revision/source/FTL.TXT" "$package/lib/netsurf/licenses/plutovg-freetype"
cp "$jpeg/README" "$package/lib/netsurf/licenses/libjpeg"
cp "$cache/bearssl-0.6/LICENSE.txt" "$package/lib/netsurf/licenses/BearSSL"
cp "$png/LICENSE" "$package/lib/netsurf/licenses/libpng"
echo "$package"
