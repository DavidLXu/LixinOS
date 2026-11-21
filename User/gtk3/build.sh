#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gtk3-port"
musl="$cache/cpython-port"
rt="$build/runtime"
prefix="$build/sysroot"
jobs=${JOBS:-8}

if [[ ! -s "$cache/lite-xl-port/pcre2-build/libpcre2-8.a" ]]; then
    bash "$root/User/lite-xl/build.sh"
fi
LIXINOS_GCC_BUILD="$rt" bash "$root/User/gcc/build.sh" rt
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$build/tools/bin:${cross[0]}/bin:$PATH"

mkdir -p "$build" "$prefix/include" "$prefix/lib/pkgconfig"
if [[ ! -x "$build/tools/bin/meson" ]] || ! "$build/tools/bin/python3" -c 'import distutils.version' 2>/dev/null; then
    python3 -m venv "$build/tools"
    "$build/tools/bin/pip" install meson==1.7.2 ninja==1.11.1.4 setuptools==80.9.0
fi
while read -r name source digest url; do
    archive="$build/$name"
    if [[ ! -s "$archive" ]]; then
        curl -fLsS --retry 3 "$url" -o "$archive.tmp"
        printf '%s  %s\n' "$digest" "$archive.tmp" | shasum -a 256 -c -
        mv "$archive.tmp" "$archive"
    fi
    printf '%s  %s\n' "$digest" "$archive" | shasum -a 256 -c -
    if [[ ! -d "$build/$source" ]]; then tar -xf "$archive" -C "$build"; fi
done < "$port/sources.tsv"

cat > "$build/cc" <<EOF_CC
#!/usr/bin/env bash
args=()
for arg do
    if [[ \$arg != -pthread ]]; then args+=("\$arg"); fi
done
exec "$rt/cc" "\${args[@]}"
EOF_CC
chmod +x "$build/cc"
cat > "$build/cross.ini" <<EOF_CROSS
[binaries]
c = '$build/cc'
cpp = '$rt/cxx'
ar = '$(command -v aarch64-none-elf-ar)'
strip = '$(command -v aarch64-none-elf-strip)'
pkg-config = '$(command -v pkg-config)'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$prefix/lib/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[built-in options]
c_args = ['-ffunction-sections', '-fdata-sections']
EOF_CROSS

export PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
pcre="$cache/lite-xl-port/pcre2-build"
cp "$pcre/pcre2.h" "$prefix/include/"
cp "$pcre/libpcre2-8.a" "$prefix/lib/"
sed "s|^prefix=.*|prefix=$prefix|" "$pcre/libpcre2-8.pc" > "$prefix/lib/pkgconfig/libpcre2-8.pc"
cat > "$prefix/lib/pkgconfig/zlib.pc" <<EOF_ZLIB
Name: zlib
Description: Static zlib in the LixinOS musl sysroot
Version: 1.3.1
Libs: -L"$musl/sysroot/lib" -lz
Cflags: -I"$musl/sysroot/include"
EOF_ZLIB

if [[ ! -s "$prefix/lib/libffi.a" ]]; then
    python3 - "$build/libffi-3.4.8/src/tramp.c" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
source = path.read_text().replace('#if defined (__linux__) || defined (__CYGWIN__)',
    '#if (defined (__linux__) && !defined (__lixinos__)) || defined (__CYGWIN__)')
if source != path.read_text():
    path.write_text(source)
PY
    mkdir -p "$build/libffi-build"
    (
        cd "$build/libffi-build"
        CC="$build/cc" AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib \
            "$build/libffi-3.4.8/configure" --host=aarch64-linux-musl \
            --prefix="$prefix" --libdir="$prefix/lib" --disable-shared --disable-docs \
            --disable-exec-static-tramp
        make -j"$jobs" install
    )
fi
meson_library() {
    local name=$1 source=$2
    shift 2
    local stage="$build/$name-build"
    if [[ -f "$stage/build.ninja" ]]; then
        meson setup --reconfigure "$stage" "$build/$source" "$@"
    else
        meson setup "$stage" "$build/$source" --cross-file "$build/cross.ini" \
            --prefix "$prefix" --libdir lib --default-library static --buildtype minsize \
            --wrap-mode nodownload "$@"
    fi
    meson compile -C "$stage" -j "$jobs"
    meson install -C "$stage" --no-rebuild
}

if ! patch -d "$build/glib-2.78.6" -p1 -R -f --dry-run < "$port/glib.patch" >/dev/null 2>&1; then
    patch -d "$build/glib-2.78.6" -p1 < "$port/glib.patch"
fi
meson_library glib glib-2.78.6 -Dtests=false -Dnls=disabled -Dxattr=false \
    -Dselinux=disabled -Dlibmount=disabled -Dlibelf=disabled

static_pc() {
    local name=$1 version=$2 libs=$3 includes=$4
    cat > "$prefix/lib/pkgconfig/$name.pc" <<EOF_PC
Name: $name
Description: $name for LixinOS
Version: $version
Libs: $libs
Cflags: $includes
EOF_PC
}

net="$cache/netsurf-port"
if [[ ! -s "$net/sysroot/lib/libpng.a" ]]; then
    bash "$root/User/netsurf/build.sh"
fi
hb="$net/harfbuzz-33a3f8de60dcad7535f14f07d6710144548853ac"
ft="$cache/lite-xl-port"
static_pc libpng 1.6.48 "-L$net/sysroot/lib -lpng -lz" "-I$net/sysroot/include"
static_pc libjpeg 9f "-L$net/sysroot/lib -ljpeg" "-I$net/sysroot/include"
static_pc freetype2 26.2.20 "-L$ft/freetype-build -lfreetype" \
    "-I$ft/freetype-build/include -I$ft/freetype-VER-2-14-1/include"
if [[ ! -s "$prefix/lib/libharfbuzz.a" ]]; then
    "$rt/cxx" -std=gnu++17 -DHAVE_PTHREAD -Os -ffunction-sections -fdata-sections \
        -c "$hb/src/harfbuzz.cc" -o "$build/harfbuzz.o"
    aarch64-none-elf-ar rcs "$prefix/lib/libharfbuzz.a" "$build/harfbuzz.o"
fi
hb_version=$(awk '/^#define HB_VERSION_STRING / {gsub(/"/, "", $3); print $3}' "$hb/src/hb-version.h")
static_pc harfbuzz "$hb_version" "-L$prefix/lib -lharfbuzz -lm" "-I$hb/src"

cmake -S "$build/expat-2.7.1" -B "$build/expat-build" \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_C_COMPILER="$build/cc" \
    -DCMAKE_AR="$(command -v aarch64-none-elf-ar)" -DCMAKE_RANLIB="$(command -v aarch64-none-elf-ranlib)" \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_C_FLAGS="-ffunction-sections -fdata-sections" \
    -DEXPAT_SHARED_LIBS=OFF -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_TESTS=OFF \
    -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_DOCS=OFF -DEXPAT_BUILD_FUZZERS=OFF
cmake --build "$build/expat-build" --parallel "$jobs"
cmake --install "$build/expat-build"

meson_library pixman pixman-0.44.2 -Dtests=disabled -Ddemos=disabled -Dgtk=disabled
meson_library fontconfig fontconfig-2.15.0 -Ddoc=disabled -Dtests=disabled \
    -Dtools=disabled -Dcache-build=disabled -Dnls=disabled
meson_library cairo cairo-1.18.4 -Dtests=disabled -Dxlib=disabled -Dxcb=disabled \
    -Dglib=enabled -Dsymbol-lookup=disabled
meson_library fribidi fribidi-1.0.16 -Ddocs=false -Dbin=false -Dtests=false
# Pango command-line tools are not part of the guest runtime.
sed -i.bak "/^subdir('utils')$/d; /^subdir('tools')$/d" "$build/pango-1.54.0/meson.build"
meson_library pango pango-1.54.0 -Dintrospection=disabled -Dbuild-testsuite=false \
    -Dbuild-examples=false -Dxft=disabled
meson_library atk atk-2.38.0 -Dintrospection=false
sed -i.bak 's/^foreach bin: gdkpixbuf_bin$/foreach bin: []/' \
    "$build/gdk-pixbuf-2.42.12/gdk-pixbuf/meson.build"
meson_library pixbuf gdk-pixbuf-2.42.12 -Dintrospection=disabled -Dtests=false \
    -Dinstalled_tests=false -Dman=false -Dtiff=disabled -Dbuiltin_loaders=all -Dgio_sniffing=false
meson_library epoxy libepoxy-1.5.10 -Dglx=no -Degl=no -Dx11=false -Dtests=false

gtk="$build/gtk-3.24.49"
cp "$port/gdk-lixinos-server.c" "$gtk/gdk/broadway/gdkbroadway-server.c"
cp "$root/User/desktop/window-client.h" "$gtk/gdk/broadway/window-client.h"
# The guest uses the window protocol in-process. GTK's Linux helper binaries
# and the separate browser server are unnecessary for this build.
sed -i.bak '/^# gtk4-broadwayd/,$d' "$gtk/gdk/broadway/meson.build"
sed -i.bak '/^####### Utils/,$d' "$gtk/gtk/meson.build"
sed -i.bak 's/GTK_PRINT_BACKENDS/""/g' "$gtk/gtk/gtksettings.c" "$gtk/gtk/gtkprintbackend.c"
python3 - "$gtk/modules/meson.build" "$gtk/gdk/broadway/gdkkeys-broadway.c" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
path.write_text(path.read_text().replace("subdir('input')", "").replace("subdir('printbackends')",
    "print_backends = []\ndisabled_print_backends = ['file', 'cups']"))
keymap = Path(sys.argv[2])
source = keymap.read_text()
if '    *consumed_modifiers = 0;' not in source:
    keymap.write_text(source.replace('    *level = 0;\n  return TRUE;',
        '    *level = 0;\n  if (consumed_modifiers)\n    *consumed_modifiers = 0;\n  return TRUE;'))
PY
# GTK must generate its embedded themes and icons using a Mac-native GLib tool.
if ! command -v glib-compile-resources >/dev/null || ! command -v glib-compile-schemas >/dev/null; then
    echo 'GTK generation requires host glib-compile-resources and glib-compile-schemas' >&2
    exit 1
fi
if ! command -v gdk-pixbuf-pixdata >/dev/null; then
    pixdata="$build/pixbuf-host/gdk-pixbuf/gdk-pixbuf-pixdata"
    if [[ ! -x "$pixdata" ]]; then
        mkdir -p "$build/host-sources"
        tar -xf "$build/pixbuf.tar.xz" -C "$build/host-sources"
        if [[ ! -f "$build/pixbuf-host/build.ninja" ]]; then
            env -u PKG_CONFIG_LIBDIR meson setup "$build/pixbuf-host" \
                "$build/host-sources/gdk-pixbuf-2.42.12" --buildtype minsize \
                -Dtests=false -Dinstalled_tests=false -Dman=false -Dintrospection=disabled \
                -Dtiff=disabled -Djpeg=disabled -Dgif=disabled -Dgio_sniffing=false -Dbuiltin_loaders=png
        fi
        env -u PKG_CONFIG_LIBDIR meson compile -C "$build/pixbuf-host" gdk-pixbuf-pixdata
    fi
    export GDK_PIXBUF_PIXDATA="$pixdata"
fi
python3 - "$prefix/lib/pkgconfig/gio-2.0.pc" \
    "$(command -v glib-compile-resources)" "$(command -v glib-compile-schemas)" <<'PY'
from pathlib import Path
import re
import sys
path = Path(sys.argv[1])
source = path.read_text()
for name, tool in zip(('glib_compile_resources', 'glib_compile_schemas'), sys.argv[2:]):
    source = re.sub(rf'^{name}=.*$', f'{name}={tool}', source, flags=re.M)
path.write_text(source)
PY
meson_library gtk gtk-3.24.49 -Dx11_backend=false -Dwayland_backend=false \
    -Dbroadway_backend=true -Dintrospection=false -Dtests=false -Dexamples=false \
    -Ddemos=false -Dprint_backends= -Dbuiltin_immodules=none

package="$build/package"
mkdir -p "$package/bin" "$package/lib/gtk3/fonts" "$package/lib/gtk3/licenses"
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$build/crt1.o"
"$musl/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I"$root/User/gcc" \
    -c "$root/User/cpython/loader.c" -o "$build/crt0.o"

read -r -a flags <<< "$(pkg-config --cflags --static gtk+-3.0 | sed 's/-pthread//g')"
read -r -a libraries <<< "$(pkg-config --libs --static gtk+-3.0 | sed 's/-pthread//g')"
"$musl/cc" -Os -ffunction-sections -fdata-sections "${flags[@]}" "$port/demo.c" \
    -nostartfiles -static -s -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
    -Wl,-T,"$root/User/rust/large.ld" "$build/crt0.o" "$build/crt1.o" \
    -Wl,--start-group "${libraries[@]}" "$rt/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group \
    -o "$package/bin/gtk3-demo.win3"
cp "$port/fonts.conf" "$package/lib/gtk3/"
cp -R "$port/mime" "$package/lib/gtk3/"
fonts="$ft/package/lib/lite-xl/data/fonts"
cp "$fonts"/{FiraSans-Regular.ttf,JetBrainsMono-Regular.ttf,NotoSansMonoCJKsc-Regular.otf} \
    "$package/lib/gtk3/fonts/"
while read -r archive source digest url; do
    for license in COPYING COPYING-LGPL-2.1 COPYING-MPL-1.1 LICENSE; do
        if [[ -f "$build/$source/$license" ]]; then
            cp "$build/$source/$license" "$package/lib/gtk3/licenses/$source-$license"
        fi
    done
done < "$port/sources.tsv"
cp "$hb/COPYING" "$package/lib/gtk3/licenses/HarfBuzz"
cp "$ft/package/lib/lite-xl/licenses/Noto" "$package/lib/gtk3/licenses/Noto"
cp "$ft/lite-xl-2.1.8/licenses/licenses.md" "$package/lib/gtk3/licenses/Bundled-fonts.md"
source "$port/build-mousepad.sh"
echo "GTK 3 package: $package"
