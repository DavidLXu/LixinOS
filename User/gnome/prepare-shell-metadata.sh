#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
build=$(cd "$port/../.." && pwd)/Kernel/target/user-build/gnome-port
work="$build/shell-native-gi"
core="$build/gjs-native-gi"
[[ -s "$core/typelib/GjsPrivate-1.0.typelib" && -s "$core/typelib/GIRepository-3.0.typelib" ]] || bash "$port/prepare-gjs-runtime.sh"
bash "$port/fetch-shell-sdk.sh"
mkdir -p "$work/gir" "$work/typelib"
cp "$core/gir/"*.gir "$work/gir/"
cp "$core/typelib/"*.typelib "$work/typelib/"

extract() {
    local package=$1 artifact=$2
    local name=${artifact##*/}
    local destination="$work/${name##*.}"
    tar -xOf "$build/$package.apk" "$artifact" > "$destination/$name"
}

desktop_metadata() {
    local package=$1
    shift
    for namespace do
        extract "$package" "usr/share/gir-1.0/$namespace.gir"
    done
}

desktop_metadata accountsservice-dev-23.13.9-r3 AccountsService-1.0
desktop_metadata at-spi2-core-dev-2.58.3-r0 Atk-1.0 Atspi-2.0
desktop_metadata gcr4-dev-4.4.0.1-r1 Gck-2 Gcr-4
desktop_metadata gdk-pixbuf-dev-2.44.4-r0 GdkPixbuf-2.0
desktop_metadata gdm-dev-48.0-r3 Gdm-1.0
desktop_metadata geoclue-dev-2.8.0-r0 Geoclue-2.0
desktop_metadata geocode-glib-dev-3.26.4-r4 GeocodeGlib-2.0
desktop_metadata gnome-desktop-dev-44.5-r0 GnomeBG-4.0 GnomeDesktop-4.0
desktop_metadata gobject-introspection-dev-1.86.0-r0 cairo-1.0 fontconfig-2.0 freetype2-2.0 \
    xlib-2.0 xfixes-4.0 GL-1.0 DBus-1.0
desktop_metadata graphene-dev-1.10.8-r5 Graphene-1.0
desktop_metadata gsettings-desktop-schemas-dev-49.1-r0 GDesktopEnums-3.0
desktop_metadata gtk4.0-dev-4.20.3-r0 Gdk-4.0 Gtk-4.0 Gsk-4.0
desktop_metadata harfbuzz-dev-11.2.1-r0 HarfBuzz-0.0
desktop_metadata ibus-dev-1.5.33-r0 IBus-1.0
desktop_metadata json-glib-dev-1.10.8-r0 Json-1.0
desktop_metadata libgweather4-dev-4.4.4-r1 GWeather-4.0
desktop_metadata networkmanager-dev-1.54.3-r0 NM-1.0
desktop_metadata librsvg-dev-2.61.2-r0 Rsvg-2.0
desktop_metadata libsoup3-dev-3.6.5-r1 Soup-3.0
desktop_metadata pango-dev-1.56.4-r0 Pango-1.0 PangoCairo-1.0
desktop_metadata polkit-dev-126-r1 Polkit-1.0 PolkitAgent-1.0
desktop_metadata upower-dev-1.91.0-r1 UPowerGlib-1.0

for module in Clutter-17 Cogl-17 Meta-17 Mtk-17; do
    extract mutter-49.4-r0 "usr/lib/mutter-17/$module.typelib"
    extract mutter-49.4-r0 "usr/lib/mutter-17/$module.gir"
done
extract gnome-shell-49.4-r0 usr/share/gnome-shell/Gvc-1.0.gir
for module in Gvc-1.0 Shell-17 St-17; do
    extract gnome-shell-49.4-r0 "usr/lib/gnome-shell/$module.typelib"
done
extract gnome-shell-49.4-r0 usr/lib/gnome-shell/girepository-1.0/Shew-0.typelib

# Plain GIR files contain source-level symbol names. Compile their runtime
# metadata without running an AArch64 library on the build host.
for gir in "$work/gir/"*.gir; do
    name=${gir##*/}
    typelib="$work/typelib/${name%.gir}.typelib"
    if [[ ! -s "$typelib" ]]; then
        gi-compile-repository --includedir="$work/gir" "$gir" -o "$typelib"
    fi
done

# Shell, St and Shew install only the compiled metadata.
for module in Shell-17 St-17 Shew-0; do
    GI_TYPELIB_PATH="$work/typelib" gi-decompile-typelib --includedir="$work/typelib" \
        "$work/typelib/$module.typelib" -o "$work/gir/$module.gir"
done

# Alpine's Shell typelib declares NM even when the local UI is disabled.
# Keep its type definitions available without linking the optional client.
rm -f "$work/gir/NM-1.0.gir"
