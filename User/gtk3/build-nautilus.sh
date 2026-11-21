#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
gtk="$cache/gtk3-port"
musl="$cache/cpython-port"
sysroot="$gtk/sysroot"
build="$gtk/nautilus-port"
package="$build/package"
jobs=${JOBS:-8}

if [[ ! -s "$sysroot/lib/pkgconfig/gtk+-3.0.pc" || ! -s "$gtk/crt0.o" ]] ||
    ! cmp -s "$port/gdk-lixinos-server.c" "$gtk/gtk-3.24.49/gdk/broadway/gdkbroadway-server.c"; then
    bash "$port/build.sh"
fi
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$gtk/tools/bin:${cross[0]}/bin:$PATH"
export PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$sysroot/lib/pkgconfig"
mkdir -p "$build" "$package/bin" "$package/lib/nautilus/schemas" "$package/lib/nautilus/licenses"

while read -r source digest url; do
    archive="$build/$source.tar.xz"
    if [[ ! -s "$archive" ]]; then
        curl -fLsS --retry 3 "$url/$source.tar.xz" -o "$archive.tmp"
        printf '%s  %s\n' "$digest" "$archive.tmp" | shasum -a 256 -c -
        mv "$archive.tmp" "$archive"
    fi
    printf '%s  %s\n' "$digest" "$archive" | shasum -a 256 -c -
    [[ -d "$build/$source" ]] || tar -xf "$archive" -C "$build"
    for license in COPYING COPYING.LIB COPYING_CCBYSA3 COPYING_LGPL LICENSE AUTHORS; do
        if [[ -f "$build/$source/$license" ]]; then
            cp "$build/$source/$license" "$package/lib/nautilus/licenses/$source-$license"
        fi
    done
done < "$port/nautilus-sources.tsv"

archive="$build/libarchive-3.8.1"
cmake -S "$archive" -B "$build/archive-build" -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_C_COMPILER="$gtk/cc" -DCMAKE_AR="$(command -v aarch64-none-elf-ar)" \
    -DCMAKE_RANLIB="$(command -v aarch64-none-elf-ranlib)" \
    -DCMAKE_INSTALL_PREFIX="$sysroot" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_C_FLAGS="-ffunction-sections -fdata-sections" \
    -DBUILD_SHARED_LIBS=OFF -DENABLE_TEST=OFF -DENABLE_INSTALL=ON \
    -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF \
    -DENABLE_OPENSSL=OFF -DENABLE_LIBB2=OFF -DENABLE_PCRE2POSIX=OFF -DENABLE_PCREPOSIX=OFF \
    -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_LZ4=OFF -DENABLE_LZO=OFF -DENABLE_ZSTD=OFF \
    -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_ICONV=OFF \
    -DZLIB_LIBRARY="$musl/sysroot/lib/libz.a" -DZLIB_INCLUDE_DIR="$musl/sysroot/include"
cmake --build "$build/archive-build" --target archive_static --parallel "$jobs"
install -m 644 "$build/archive-build/libarchive/libarchive.a" "$sysroot/lib/libarchive.a"
install -m 644 "$archive/libarchive/"{archive.h,archive_entry.h} "$sysroot/include/"
install -m 644 "$build/archive-build/build/pkgconfig/libarchive.pc" "$sysroot/lib/pkgconfig/libarchive.pc"

autoar="$build/gnome-autoar-0.4.5"
sed -i.bak -E 's/shared_library\(/static_library(/; /^  version: gnome_autoar_libversion,/d' "$autoar/gnome-autoar/meson.build"
if [[ -f "$build/autoar-build/build.ninja" ]]; then
    meson setup --reconfigure "$build/autoar-build" "$autoar"
else
    meson setup "$build/autoar-build" "$autoar" --cross-file "$gtk/cross.ini" \
        --prefix "$sysroot" --libdir lib --default-library static --buildtype minsize \
        -Dgtk=false -Dintrospection=disabled
fi
meson compile -C "$build/autoar-build" -j "$jobs"
meson install -C "$build/autoar-build" --no-rebuild

install -m 644 "$build/gsettings-desktop-schemas-3.24.1/headers/gdesktop-enums.h" "$sysroot/include/"

src="$build/nautilus-3.24.2"
if ! patch -d "$src" -p1 -R -f --dry-run < "$port/nautilus.patch" >/dev/null 2>&1; then
    patch -d "$src" -p1 < "$port/nautilus.patch"
fi

# Use the same large application layout and startup as GTK3 demo and Mousepad.
cat > "$build/cc" <<EOF_CC
#!/usr/bin/env bash
args=()
compile=false
for arg do
    case "\$arg" in
        -pthread) continue ;;
        -c|-S|-E|-M|-MM|-dump*|-print*|--version) compile=true ;;
    esac
    args+=("\$arg")
done
if "\$compile"; then
    exec "$musl/cc" -D__lixinos__ "\${args[@]}"
fi
exec "$musl/cc" -D__lixinos__ -nostartfiles -static -s \
    -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096,-T,"$root/User/rust/large.ld" \
    "$gtk/crt0.o" "$gtk/crt1.o" "\${args[@]}" \
    -Wl,--start-group "$gtk/runtime/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group
EOF_CC
chmod +x "$build/cc"
sed "s|^c = .*|c = '$build/cc'|" "$gtk/cross.ini" > "$build/cross.ini"
printf "[binaries]\ngdbus-codegen = '%s'\n" "$(command -v gdbus-codegen)" > "$build/build-tools.ini"
if [[ -f "$build/app/build.ninja" ]]; then
    meson setup --reconfigure "$build/app" "$src" -Dprefer_static=true
else
    meson setup "$build/app" "$src" --cross-file "$build/cross.ini" \
        --native-file "$build/build-tools.ini" --prefix /lib/nautilus \
        -Dprefer_static=true --default-library static --buildtype minsize --wrap-mode nodownload \
        -Denable-tracker=false -Denable-desktop=false -Denable-nst-extension=false \
        -Denable-selinux=false -Denable-packagekit=false
fi
meson compile -C "$build/app" -j "$jobs" nautilus:executable
install -m 755 "$build/app/src/nautilus" "$package/bin/nautilus.win3"

schemas="$package/lib/nautilus/schemas"
cp "$src/data/org.gnome.nautilus.gschema.xml" "$gtk/gtk-3.24.49/gtk/"org.gtk.Settings.*.gschema.xml "$schemas/"
desktop_schemas="$build/gsettings-desktop-schemas-3.24.1/schemas"
for schema in "$desktop_schemas/"*.gschema.xml.in; do
    cp "$schema" "$schemas/$(basename "${schema%.in}")"
done
glib-mkenums --comments '<!-- @comment@ -->' --fhead '<schemalist>' \
    --vhead "  <@type@ id='org.gnome.desktop.@EnumName@'>" \
    --vprod "    <value nick='@valuenick@' value='@valuenum@'/>" \
    --vtail '  </@type@>' --ftail '</schemalist>' \
    "$build/gsettings-desktop-schemas-3.24.1/headers/gdesktop-enums.h" > "$schemas/org.gnome.desktop.enums.xml"
glib-compile-schemas --strict "$schemas"
sed 's|/lib/gtk3/|/lib/nautilus/|g' "$port/fonts.conf" > "$package/lib/nautilus/fonts.conf"
mkdir -p "$package/lib/nautilus/fonts"
cp "$gtk/package/lib/gtk3/fonts/"{FiraSans-Regular.ttf,JetBrainsMono-Regular.ttf,NotoSansMonoCJKsc-Regular.otf} \
    "$package/lib/nautilus/fonts/"
cp -R "$gtk/package/lib/gtk3/licenses/." "$package/lib/nautilus/licenses/"

share="$package/lib/nautilus/share"
theme="$share/icons/Adwaita"
adwaita="$build/adwaita-icon-theme-3.24.0/Adwaita"
mkdir -p "$theme/16x16" "$theme/48x48"
cat > "$theme/index.theme" <<'EOF_THEME'
[Icon Theme]
Name=Adwaita
Directories=16x16,48x48
Inherits=hicolor

[16x16]
Size=16
Type=Fixed

[48x48]
Size=48
Type=Fixed
EOF_THEME
for name in bookmark-new dialog-question dialog-warning edit-clear edit-find folder folder-new \
    go-down go-next go-previous go-up list-add media-eject network-server object-select open-menu \
    pan-down pan-end pan-start process-stop tab-new user-desktop user-home user-trash view-grid \
    view-list window-close x-office-calendar zoom-in zoom-out; do
    icon=("$adwaita"/scalable/*/"$name-symbolic.svg")
    rsvg-convert --width 16 --height 16 "${icon[0]}" --output "$theme/16x16/$name-symbolic.png"
done
for name in folder folder-documents folder-music folder-pictures folder-videos user-desktop \
    user-home user-trash text-x-generic image-x-generic video-x-generic audio-x-generic package-x-generic; do
    icon=("$adwaita"/48x48/*/"$name.png")
    install -m 644 "${icon[0]}" "$theme/48x48/"
done
cp -R "$port/mime" "$share/"
echo "Nautilus package: $package"
