#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gtk3-port"
extra="$build/extra"
sysroot="$build/sysroot"
musl="$cache/cpython-port"
package="$extra/ristretto-package"
stage="$extra/ristretto-generated"

[[ -f "$sysroot/lib/pkgconfig/gtk+-3.0.pc" ]] || bash "$port/build.sh"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$build/tools/bin:${cross[0]}/bin:$PATH"
export PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$sysroot/lib/pkgconfig"
export GDBUS_CODEGEN="$sysroot/bin/gdbus-codegen"
mkdir -p "$extra" "$stage" "$package/bin" "$package/lib/ristretto/schemas"

while read -r source suffix hash url; do
    archive="$extra/$source.$suffix"
    if [[ ! -f "$archive" ]]; then
        curl -fLsS --retry 3 "$url/$source.$suffix" -o "$archive.tmp"
        printf '%s  %s\n' "$hash" "$archive.tmp" | shasum -a 256 -c -
        mv "$archive.tmp" "$archive"
    fi
    printf '%s  %s\n' "$hash" "$archive" | shasum -a 256 -c -
    [[ -d "$extra/$source" ]] || tar -xf "$archive" -C "$extra"
    [[ ! -f "$extra/$source/COPYING" ]] || cp "$extra/$source/COPYING" "$package/lib/ristretto/COPYING-$source"
done < "$port/ristretto-sources.tsv"

build_library() {
    local source=$1
    local component=$2
    shift 2
    local work="$extra/$source-build"
    mkdir -p "$work"
    if [[ ! -f "$work/Makefile" ]]; then
        (
            cd "$work"
            CC="$build/cc" AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib \
                CFLAGS="-Os -ffunction-sections -fdata-sections" \
                "$extra/$source/configure" --host=aarch64-linux-musl --prefix="$sysroot" \
                --enable-static --disable-shared --disable-nls "$@"
        )
    fi
    make -C "$work/$component" -j"${JOBS:-8}" install "GDBUS_CODEGEN=$GDBUS_CODEGEN"
}

build_library libxfce4util-4.20.1 . --enable-introspection=no
build_library xfconf-4.20.0 . --enable-introspection=no --disable-gsettings-backend
# libxfce4ui's dialogs and file utilities operate through GDK. Permit them
# to build with neither of the X11 and Wayland integrations.
sed -i.bak '/as_fn_error .*Either both X11 and Wayland support was disabled/s/.*/  :/' \
    "$extra/libxfce4ui-4.20.2/configure"
build_library libxfce4ui-4.20.2 libxfce4ui --disable-x11 --disable-wayland --disable-gladeui2 --enable-introspection=no
# The libtool export filter excludes objects from this static archive.
ui_archive="$sysroot/lib/libxfce4ui-2.a"
rm -f "$ui_archive.tmp"
aarch64-none-elf-ar rcs "$ui_archive.tmp" "$extra/libxfce4ui-4.20.2-build/libxfce4ui/"libxfce4ui_2_la-*.o
mv "$ui_archive.tmp" "$ui_archive"
build_library exo-4.20.0 exo
build_library libexif-0.6.26 .

upstream="$extra/ristretto-0.14.0"
sed -e 's/@TUMBLER_SERVICE_NAME_PREFIX@/org.freedesktop.thumbnails/g' \
    -e 's|@TUMBLER_SERVICE_PATH_PREFIX@|/org/freedesktop/thumbnails|g' \
    "$upstream/src/tumbler-service-dbus.xml.in" > "$stage/tumbler-service-dbus.xml"
sed 's/@REVISION@/LixinOS/' "$upstream/xfce-revision.h.in" > "$stage/xfce-revision.h"
(
    cd "$stage"
    "$GDBUS_CODEGEN" --interface-prefix org.freedesktop.thumbnails --c-namespace Tumbler \
        --generate-c-code tumbler tumbler-service-dbus.xml
    glib-genmarshal --prefix _rstto_marshal --internal --header "$upstream/src/marshal.list" > marshal.h
    glib-genmarshal --prefix _rstto_marshal --internal --body "$upstream/src/marshal.list" > marshal.c
    glib-compile-resources "$upstream/src/ristretto.gresource.xml" \
        --sourcedir "$upstream/src" --generate-source --target ristretto-resources.c
)
sed -e '/    xfce_textdomain (/i\
    g_setenv ("GDK_BACKEND", "broadway", TRUE);\
    g_setenv ("FONTCONFIG_FILE", "/lib/gtk3/fonts.conf", TRUE);\
    g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);\
    g_setenv ("GSETTINGS_SCHEMA_DIR", "/lib/ristretto/schemas", TRUE);\
    g_setenv ("NO_AT_BRIDGE", "1", TRUE);\
    const gchar * const *current_mime_dirs = g_content_type_get_mime_dirs ();\
    GPtrArray *mime_dirs = g_ptr_array_new_with_free_func (g_free);\
    for (guint i = 0; current_mime_dirs[i] != NULL; ++i)\
        g_ptr_array_add (mime_dirs, g_strdup (current_mime_dirs[i]));\
    g_ptr_array_add (mime_dirs, g_strdup ("/lib/gtk3/mime"));\
    g_ptr_array_add (mime_dirs, NULL);\
    g_content_type_set_mime_dirs ((const gchar * const *) mime_dirs->pdata);\
    g_ptr_array_unref (mime_dirs);\
' -e '/        window = rstto_main_window_new (/a\
        gtk_window_set_decorated (GTK_WINDOW (window), FALSE);\
' -e 's/xfconf_disabled = !xfconf_init (\&error);/xfconf_disabled = TRUE;/' \
    -e '/^    if (xfconf_disabled)$/,/^    }$/d' "$upstream/src/main.c" > "$stage/main.c"
sed 's/thumbnailer->priv->connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, \&error);/thumbnailer->priv->connection = NULL;/' \
    "$upstream/src/thumbnailer.c" > "$stage/thumbnailer.c"
deps=(gtk+-3.0 gio-unix-2.0 libexif libxfce4ui-2 libxfce4util-1.0 libxfconf-0 exo-2)
read -r -a flags <<< "$(pkg-config --cflags --static "${deps[@]}" | sed 's/-pthread//g')"
read -r -a libs <<< "$(pkg-config --libs --static "${deps[@]}" | sed 's/-pthread//g')"
sources=()
for source in "$upstream/src/"*.c; do
    case "${source##*/}" in
        main.c|thumbnailer.c) ;;
        *) sources+=("$source") ;;
    esac
done
"$musl/cc" -Os -ffunction-sections -fdata-sections "${flags[@]}" -I"$stage" -I"$upstream/src" \
    -DPACKAGE='"ristretto"' -DPACKAGE_NAME='"Ristretto"' -DVERSION='"0.14.0"' -DVERSION_SHORT='"0.14"' \
    -DLOCALEDIR='"/lib/ristretto/locale"' -DGETTEXT_PACKAGE='"ristretto"' -DCOPYRIGHT_YEAR='"2026"' \
    -DHAVE_XFCE_REVISION_H=1 -DTUMBLER_SERVICE_NAME_PREFIX='"org.freedesktop.thumbnails"' \
    -DTUMBLER_SERVICE_PATH_PREFIX='"/org/freedesktop/thumbnails"' \
    "${sources[@]}" "$stage/"*.c -nostartfiles -static -s -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
    -Wl,-T,"$root/User/rust/large.ld" "$build/crt0.o" "$build/crt1.o" \
    -Wl,--start-group "${libs[@]}" "$build/runtime/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group \
    -o "$package/bin/ristretto.win3"
cp "$build/gtk-3.24.49/gtk/"org.gtk.Settings.*.gschema.xml "$package/lib/ristretto/schemas/"
glib-compile-schemas --strict "$package/lib/ristretto/schemas"
echo "Ristretto package: $package"
