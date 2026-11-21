#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/lite-xl-port"
musl="$cache/cpython-port"
runtime="$build/runtime"
jobs=${JOBS:-8}
mkdir -p "$build"
LIXINOS_GCC_BUILD="$runtime" bash "$root/User/gcc/build.sh" rt
if ! command -v aarch64-none-elf-gcc >/dev/null; then
    cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
    export PATH="${cross[0]}/bin:$PATH"
fi

source_archive() {
    local file=$1 directory=$2 sha=$3 url=$4
    if [[ ! -f "$build/$file" ]]; then
        curl -fsSL --retry 3 "$url" -o "$build/$file.tmp"
        printf '%s  %s\n' "$sha" "$build/$file.tmp" | shasum -a 256 -c -
        mv "$build/$file.tmp" "$build/$file"
    else
        printf '%s  %s\n' "$sha" "$build/$file" | shasum -a 256 -c -
    fi
    if [[ ! -d "$build/$directory" ]]; then tar -xf "$build/$file" -C "$build"; fi
}
source_archive lite-xl.tar.gz lite-xl-2.1.8 fcaffb946bc60583369cb040d533a4ac18075a6d474d49a2a5ff4bf87e2e9a10 \
    https://codeload.github.com/lite-xl/lite-xl/tar.gz/refs/tags/v2.1.8
source_archive sdl.tar.gz SDL-release-3.2.24 66c374ee7f786abcd8592d18fc5b18b1e3f1c67e923867eee5f9c503cb253afd \
    https://codeload.github.com/libsdl-org/SDL/tar.gz/refs/tags/release-3.2.24
source_archive lua.tar.gz lua-5.4.8 4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae \
    https://www.lua.org/ftp/lua-5.4.8.tar.gz
source_archive freetype.tar.gz freetype-VER-2-14-1 44bd69d1f0750603410cfd4f26f1c5523c5a3a087a2dfa8639b757dc7c6677be \
    https://codeload.github.com/freetype/freetype/tar.gz/refs/tags/VER-2-14-1
source_archive pcre2.tar.gz pcre2-pcre2-10.46 ffde0c6ead457c4f8edcd8f9ce490bbabd30adabdfd179ecaddd320946cf3872 \
    https://codeload.github.com/PCRE2Project/pcre2/tar.gz/refs/tags/pcre2-10.46

python3 - "$build/SDL-release-3.2.24/src" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
header = root / "dynapi/SDL_dynapi.h"
source = header.read_text()
if "|| defined(__lixinos__)" not in source:
    header.write_text(source.replace("#if defined(SDL_PLATFORM_PRIVATE)",
                                    "#if defined(SDL_PLATFORM_PRIVATE) || defined(__lixinos__)"))
timer = root / "timer/unix/SDL_systimer.c"
source = timer.read_text()
if "#if defined(CLOCK_MONOTONIC_RAW) && !defined(__lixinos__)" not in source:
    timer.write_text(source.replace("#ifdef CLOCK_MONOTONIC_RAW",
                                   "#if defined(CLOCK_MONOTONIC_RAW) && !defined(__lixinos__)"))
PY

cat > "$build/cross-cc" <<SH
#!/usr/bin/env bash
args=()
for arg do
    if [[ \$arg != -pthread ]]; then args+=("\$arg"); fi
done
exec "$runtime/cc" "\${args[@]}"
SH
chmod +x "$build/cross-cc"
common_cmake=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
    -DCMAKE_C_COMPILER="$build/cross-cc" -DCMAKE_ASM_COMPILER="$build/cross-cc"
    -DCMAKE_AR="$(command -v aarch64-none-elf-ar)" -DCMAKE_RANLIB="$(command -v aarch64-none-elf-ranlib)"
    -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_C_FLAGS="-ffunction-sections -fdata-sections"
    -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON -DBUILD_SHARED_LIBS=OFF)
libbuild() {
    local dir=$1 output=$2
    shift 2
    cmake -S "$build/$dir" -B "$build/$output" "${common_cmake[@]}" "$@"
    cmake --build "$build/$output" --parallel "$jobs"
}
libbuild SDL-release-3.2.24 sdl-build -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TEST_LIBRARY=OFF -DSDL_AUDIO=OFF -DSDL_RENDER=OFF -DSDL_GPU=OFF \
    -DSDL_CAMERA=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF \
    -DSDL_SENSOR=OFF -DSDL_DIALOG=OFF -DSDL_POWER=OFF -DSDL_OFFSCREEN=OFF \
    -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_RPI=OFF \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF -DSDL_DBUS=OFF \
    -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF -DSDL_LIBURING=OFF -DSDL_PIPEWIRE=OFF \
    -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_PTHREADS_SEM=OFF
libbuild freetype-VER-2-14-1 freetype-build -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON \
    -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
libbuild pcre2-pcre2-10.46 pcre2-build -DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF

work=$(mktemp -d "$build/editor.XXXXXX")
trap 'rm -rf "$work"' EXIT
tar -xf "$build/lite-xl.tar.gz" -C "$work"
src="$work/lite-xl-2.1.8"
patch -d "$src" -p1 < "$port/lixinos.patch"
lua="$build/lua-5.4.8/src"
sdl="$build/SDL-release-3.2.24"
freetype="$build/freetype-VER-2-14-1"
pcre="$build/pcre2-build"
common=(-Os -std=gnu11 -ffunction-sections -fdata-sections)
sources=()
for file in "$lua"/*.c; do
    case "${file##*/}" in lua.c|luac.c) continue;; esac
    sources+=("$file")
done
(
    cd "$work"
    "$musl/cc" "${common[@]}" -DLUA_USE_POSIX -c "${sources[@]}"
    aarch64-none-elf-ar rcs liblua.a ./*.o
)
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$work/crt1.o"
"$musl/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION \
    -I"$root/User/gcc" -c "$root/User/cpython/loader.c" -o "$work/crt0.o"
"$musl/cc" "${common[@]}" -D_GNU_SOURCE -DPCRE2_STATIC \
    '-DLITE_PROJECT_VERSION_STR="2.1.8"' '-DLITE_ARCH_TUPLE="aarch64-lixinos"' \
    -I"$sdl/include" -I"$lua" -I"$build/freetype-build/include" -I"$freetype/include" \
    -I"$pcre" -I"$port" -I"$src/src" \
    "$port/window.c" "$src"/src/*.c "$src"/src/api/*.c "$src/src/api/dirmonitor/dummy.c" \
    -nostartfiles -static -s -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
    -Wl,-T,"$root/User/gcc/application.ld" "$work/crt0.o" "$work/crt1.o" \
    -Wl,--start-group "$build/sdl-build/libSDL3.a" "$build/freetype-build/libfreetype.a" \
    "$pcre/libpcre2-8.a" "$work/liblua.a" "$runtime/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group \
    -o "$work/lite-xl.win3"
font_file() {
    local name=$1 sha=$2 path=$3
    if [[ ! -f "$build/$name" ]]; then
        curl -fsSL --retry 3 "https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/$path" -o "$work/$name"
        printf '%s  %s\n' "$sha" "$work/$name" | shasum -a 256 -c -
        mv "$work/$name" "$build/$name"
    else
        printf '%s  %s\n' "$sha" "$build/$name" | shasum -a 256 -c -
    fi
}
font_file NotoSansMonoCJKsc-Regular.otf ec04cc376b34887cedbdf84074e2e226ed2761eeabdcb9173fc1dd7bfd153ef7 \
    Sans/Mono/NotoSansMonoCJKsc-Regular.otf
font_file noto-license.txt 6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2 LICENSE
mkdir -p "$work/package/bin" "$work/package/lib/lite-xl/licenses"
lib="$work/package/lib/lite-xl"
cp -R "$src/data" "$lib/data"
cp "$build/NotoSansMonoCJKsc-Regular.otf" "$lib/data/fonts/"
cp "$src/LICENSE" "$lib/licenses/Lite-XL"
cp "$sdl/LICENSE.txt" "$lib/licenses/SDL3"
cp "$freetype/docs/FTL.TXT" "$lib/licenses/FreeType"
cp "$build/pcre2-pcre2-10.46/LICENCE.md" "$lib/licenses/PCRE2"
cp "$build/lua-5.4.8/doc/readme.html" "$lib/licenses/Lua.html"
cp "$build/noto-license.txt" "$lib/licenses/Noto"
mv "$work/lite-xl.win3" "$work/package/bin/"
mkdir -p "$build/package"
cp -R "$work/package/." "$build/package/"
echo "Lite XL package: $build/package"
