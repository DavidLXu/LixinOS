#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gnome-port"
gtk="$cache/gtk3-port"
wl="$cache/wayland-port/sdk"
prefix="$build/sysroot"

mkdir -p "$build" "$prefix" "$build/alpine-sdk"
while read -r name digest url; do
    [[ -n $name && $name != \#* ]] || continue
    archive="$build/$name"
    if [[ ! -s "$archive" ]]; then
        curl -fLsS --retry 3 "$url" -o "$archive.tmp"
        printf '%s  %s\n' "$digest" "$archive.tmp" | shasum -a 256 -c -
        mv "$archive.tmp" "$archive"
    fi
    printf '%s  %s\n' "$digest" "$archive" | shasum -a 256 -c -
    case $name in
        *.tar.xz|*.tar.gz|*.tar.bz2)
            [[ -d "$build/${name%.tar.*}" ]] || tar -xf "$archive" -C "$build"
            ;;
        *.apk)
            [[ -f "$build/alpine-sdk/.$name" ]] || {
                tar -xf "$archive" -C "$build/alpine-sdk"
                touch "$build/alpine-sdk/.$name"
            }
            ;;
    esac
done < "$port/sources.tsv"

[[ -x "$gtk/tools/bin/meson" && -f "$gtk/sysroot/lib/libpcre2-8.a" ]] || bash "$root/User/gtk3/build.sh"
[[ -f "$wl/lib/libwayland-egl.a" ]] || bash "$root/User/wayland/build.sh"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$wl/bin:$gtk/tools/bin:${cross[0]}/bin:/opt/homebrew/opt/bison/bin:$PATH"
LIXINOS_GCC_BUILD="$build/runtime" bash "$root/User/gcc/build.sh" rt

alpine="$build/alpine-sdk/usr"
stdcpp="$alpine/include/c++/14.2.0"
musllib="$cache/cpython-port/sysroot/lib"
crtbegin=$(aarch64-none-elf-gcc -print-file-name=crtbegin.o)
crtend=$(aarch64-none-elf-gcc -print-file-name=crtend.o)
cat > "$build/cc" <<EOF_CC
#!/usr/bin/env bash
args=()
cpp_link=false
for arg do
    case \$arg in
        -pthread) continue ;;
        -lstdc++) cpp_link=true ;;
        */libstdc++.a) arg="$alpine/lib/libstdc++.a"; cpp_link=true ;;
    esac
    args+=("\$arg")
done
if \$cpp_link; then
    args=(-L"$musllib" -L"$alpine/lib" "$crtbegin" "\${args[@]}" "$crtend")
fi
if [[ -f "$prefix/include/gsettings-desktop-schemas/gdesktop-enums.h" ]]; then
    args=(-I"$prefix/include/gsettings-desktop-schemas" "\${args[@]}")
fi
if [[ -f "$prefix/include/atk-1.0/atk/atk.h" ]]; then
    args=(-I"$prefix/include/atk-1.0" "\${args[@]}")
fi
exec "$build/runtime/cc" -idirafter "$alpine/include" "\${args[@]}"
EOF_CC
cat > "$build/cxx" <<EOF_CXX
#!/usr/bin/env bash
for arg do
    case \$arg in -c|-E|-S|-M|-MM|-print*|--version|-dump*) compile=true;; esac
done
args=()
for arg do
    case \$arg in *.c) args+=(-x c++ "\$arg" -x none);; *) args+=("\$arg");; esac
done
if [[ \${compile:-false} == false ]]; then
    args+=(-lstdc++ -lm)
fi
exec "$build/cc" -isystem "$stdcpp" -isystem "$stdcpp/aarch64-alpine-linux-musl" "\${args[@]}"
EOF_CXX
chmod +x "$build/cc" "$build/cxx"

# Target pkg-config must never select workstation libraries.
pkgdirs="$prefix/lib/pkgconfig:$prefix/share/pkgconfig:$gtk/sysroot/lib/pkgconfig:$wl/lib/pkgconfig:$cache/dbus-port/sdk/lib/pkgconfig"
cat > "$build/cross.ini" <<EOF_CROSS
[binaries]
c = '$build/cc'
cpp = '$build/cxx'
ar = '$(command -v aarch64-none-elf-ar)'
strip = '$(command -v aarch64-none-elf-strip)'
pkg-config = '$(command -v pkg-config)'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$pkgdirs'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[built-in options]
c_args = ['-ffunction-sections', '-fdata-sections']
cpp_args = ['-ffunction-sections', '-fdata-sections']
b_ndebug = 'true'
EOF_CROSS

meson_library() {
    local name=$1 source=$2
    shift 2
    local stage="$build/$name" path="$build/$source"
    if [[ -f "$stage/build.ninja" ]]; then
        meson setup --reconfigure "$stage" "$path" "$@"
    else
        meson setup "$stage" "$path" --cross-file "$build/cross.ini" \
            --prefix "$prefix" --libdir lib --default-library static --buildtype minsize \
            --wrap-mode nodownload "$@"
    fi
    meson compile -C "$stage" -j "${JOBS:-8}"
    meson install --no-rebuild -C "$stage"
}

glib="$build/glib-2.86.0"
bash "$port/build-ffi.sh"
install -m 644 "$port/gmodule-lixinos.c" "$port/static-modules.h" "$glib/gmodule/"
for diff in "$root/User/gtk3/glib.patch" "$port/glib-lixinos.patch" "$port/glib-pollable.patch"; do
    if ! patch -f -Rs --dry-run -p1 -d "$glib" < "$diff" >/dev/null 2>&1; then
        patch -f -s -p1 -d "$glib" < "$diff"
    fi
done
meson_library glib-cross glib-2.86.0 -Dtests=false -Dnls=disabled -Dxattr=false -Dselinux=disabled \
    -Dlibmount=disabled -Dlibelf=disabled -Dintrospection=disabled -Dsysprof=disabled
bash "$port/bind-glib-sdk.sh" "$prefix"
meson_library harfbuzz-cross ../netsurf-port/harfbuzz-33a3f8de60dcad7535f14f07d6710144548853ac \
    -Dglib=enabled -Dfreetype=enabled -Dgobject=disabled -Dicu=disabled -Dcairo=disabled \
    -Dchafa=disabled -Dtests=disabled -Ddocs=disabled -Dutilities=disabled -Dintrospection=disabled
meson_library pango-cross pango-1.56.4 -Dprefer_static=true -Dintrospection=disabled \
    -Dbuild-testsuite=false -Dbuild-examples=false -Dxft=disabled -Dlibthai=disabled

meson_library libdrm-cross libdrm-2.4.124 -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled \
    -Dnouveau=disabled -Dvmwgfx=disabled -Dfreedreno=disabled -Dvc4=disabled -Detnaviv=disabled \
    -Dcairo-tests=disabled -Dvalgrind=disabled -Dman-pages=disabled -Dtests=false

meson_library graphene-cross graphene-1.10.8 -Dtests=false -Dintrospection=disabled -Dgtk_doc=false
meson_library lcms-cross Little-CMS-lcms2.16 -Djpeg=disabled -Dtiff=disabled
xkb="$build/libxkbcommon-xkbcommon-1.8.1"
if ! patch -f -Rs --dry-run -p1 -d "$xkb" < "$port/xkbcommon-files.patch" >/dev/null 2>&1; then
    patch -f -s -p1 -d "$xkb" < "$port/xkbcommon-files.patch"
fi
meson_library xkbcommon-cross libxkbcommon-xkbcommon-1.8.1 -Denable-tools=false -Denable-x11=false \
    -Denable-wayland=false -Denable-xkbregistry=false -Denable-bash-completion=false \
    -Dxkb-config-root=/lib/gnome/xkb
meson_library schemas-cross gsettings-desktop-schemas-49.0 -Dintrospection=false
meson_library atk-cross at-spi2-core-2.58.0 -Datk_only=true -Dintrospection=disabled \
    -Dx11=disabled -Duse_systemd=false

if ! "$gtk/tools/bin/python3" -c 'import jinja2'; then
    "$gtk/tools/bin/pip" install Jinja2==3.1.6
fi
meson_library libei-cross libei-1.4.0 -Dliboeffis=disabled -Dtests=disabled

colord="$build/colord-1.4.7"
if ! patch -f -Rs --dry-run -p1 -d "$colord" < "$port/colord-client.patch" >/dev/null 2>&1; then
    patch -f -s -p1 -d "$colord" < "$port/colord-client.patch"
fi
meson_library colord-cross colord-1.4.7 -Ddaemon=false -Dsystemd=false -Dudev_rules=false \
    -Dintrospection=false -Dargyllcms_sensor=false -Dbash_completion=false -Dtests=false \
    -Ddocs=false -Dman=false -Dpnp_ids=/lib/gnome/pnp.ids
mkdir -p "$build/host-pc"
cat > "$build/host-pc/hwdata.pc" <<PC
pkgdatadir=$build
Name: hwdata
Description: Hardware vendor identification data
Version: 0.393
PC
PKG_CONFIG_PATH_FOR_BUILD="$build/host-pc" meson_library display-info-cross libdisplay-info-0.2.0

# The native scanner generates the protocol enums shipped with the XML.
export PKG_CONFIG_PATH="$wl/lib/pkgconfig"
protocol_stage="$build/protocols"
if [[ ! -f "$protocol_stage/build.ninja" ]]; then
    meson setup "$protocol_stage" "$build/wayland-protocols-1.45" --prefix "$prefix" \
        --libdir lib -Dtests=false --wrap-mode nodownload
fi
meson compile -C "$protocol_stage" -j "${JOBS:-8}"
meson install --no-rebuild -C "$protocol_stage"
echo "GNOME dependency sysroot: $prefix"
