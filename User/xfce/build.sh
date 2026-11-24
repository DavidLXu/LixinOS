#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
gtk="$cache/gtk3-port"
build="$cache/xfce-port"
sys="$gtk/sysroot"
pkg="$build/package"
[[ -f "$sys/lib/pkgconfig/libxfce4ui-2.pc" ]] || bash "$root/User/gtk3/build-ristretto.sh"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$gtk/tools/bin:${cross[0]}/bin:$PATH"
export PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$sys/lib/pkgconfig"
if [[ ! -f "$sys/lib/pkgconfig/libxfce4kbd-private-3.pc" || ! -s "$sys/lib/libxfce4kbd-private-3.a" ]]; then
    make -C "$gtk/extra/libxfce4ui-4.20.2-build/libxfce4kbd-private" -j"${JOBS:-6}" install
fi
mkdir -p "$pkg/bin" "$pkg/lib/xfce/applications" "$build"
cd "$build"
fetch() {
    local archive="$1.tar.bz2"
    if [[ ! -f "$archive" ]]; then
        curl -fLsS --retry 3 "https://archive.xfce.org/src/xfce/$2/${4:-4.20}/$archive" -o "$archive.tmp"
        printf '%s  %s\n' "$3" "$archive.tmp" | shasum -a 256 -c -
        mv "$archive.tmp" "$archive"
    fi
    printf '%s  %s\n' "$3" "$archive" | shasum -a 256 -c -
    [[ -d "$1" ]] || tar xf "$archive"
}
panel=xfce4-panel-4.20.8
garcon=garcon-4.20.0
fetch "$panel" xfce4-panel d69cb1f377953aeb1fb9bdbcef12c246bea66586e3f2868f3b758e0e8ce3d3fe
fetch "$garcon" garcon 7fb8517c12309ca4ddf8b42c34bc0c315e38ea077b5442bfcc4509415feada8f
desktop=xfdesktop-4.18.1
fetch "$desktop" xfdesktop ef9268190c25877e22a9ff5aa31cc8ede120239cb0dfca080c174e7eed4ff756 4.18
# The shared header imports Xlib even for its display-independent functions.
mkdir -p backdrop
sed -e '/#include <X11\/Xlib.h>/d' -e 's@DATADIR "/backgrounds/xfce/xfce-shapes.svg"@"/lib/xfce/xfce-blue.jpg"@' \
    "$desktop/common/xfdesktop-common.h" > backdrop/xfdesktop-common.h
sed '/#include <gdk\/gdkx.h>/d' "$desktop/common/xfdesktop-common.c" > backdrop/xfdesktop-common.c
mkdir -p garcon-build
(
    cd garcon-build
    if [[ ! -f Makefile ]]; then
        CC="$gtk/cc" AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib \
            "../$garcon/configure" --host=aarch64-linux-musl --prefix="$sys" \
            --enable-static --disable-shared --disable-nls --enable-introspection=no
    fi
    make -C garcon -j"${JOBS:-6}" install
)
read -r -a cflags <<< "$(pkg-config --cflags --static gtk+-3.0 garcon-1 libxfce4ui-2 libxfconf-0 | sed 's/-pthread//g')"
read -r -a libs <<< "$(pkg-config --libs --static gtk+-3.0 garcon-1 libxfce4ui-2 libxfconf-0 | sed 's/-pthread//g')"
cc="$cache/cpython-port/cc"
flags=(-Os -ffunction-sections -fdata-sections -std=gnu11)
link=(-nostartfiles -static -s -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096)
runtime=("$gtk/crt0.o" "$gtk/crt1.o")
# The digital clock's hierarchy hook migrates old xfconf plugin preferences.
# The native session statically embeds the clock and has no D-Bus channel.
sed '/g_signal_connect (digital, "hierarchy-changed"/d' "$panel/plugins/clock/clock-digital.c" > clock-digital.c
"$cc" "${flags[@]}" "${cflags[@]}" -I"$panel" -I"$panel/plugins/clock" -Ibackdrop -I"$desktop/src" -DHAVE_STRING_H -DHAVE_STDLIB_H \
    -DHAVE_MATH_H -DPACKAGE_NAME='"xfce4-panel"' -DGETTEXT_PACKAGE='"xfce4-panel"' -DPACKAGE_VERSION='"4.20.8"' -DLIBXFCE4PANEL_COMPILATION \
    "$port/panel.c" backdrop/xfdesktop-common.c "$desktop/src/"{xfce-backdrop,xfce-desktop-enum-types}.c \
    "$panel/panel/panel-itembar.c" "$panel/common/panel-debug.c" \
    "$panel/libxfce4panel/"{libxfce4panel-enum-types,xfce-panel-plugin-provider}.c \
    "$panel/plugins/clock/"{clock-time,clock-sleep-monitor}.c clock-digital.c \
    "${link[@]}" -Wl,-T,"$root/User/rust/large.ld" "${runtime[@]}" \
    -Wl,--start-group "${libs[@]}" "$gtk/runtime/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group \
    -o "$build/panel.tmp"
mv "$build/panel.tmp" "$pkg/lib/xfce/panel.win3"
"$cc" "${flags[@]}" "$port/session.c" "$root/User/tcc/api.c" "${link[@]}" \
    -Wl,-T,"$root/User/gcc/application.ld" "${runtime[@]}" \
    -Wl,--start-group "$gtk/runtime/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group -o "$build/session.tmp"
mv "$build/session.tmp" "$pkg/bin/startxfce4"
cp "$panel/COPYING" "$pkg/lib/xfce/COPYING-panel"
cp "$panel/COPYING.LIB" "$pkg/lib/xfce/COPYING-panel-lib"
cp "$desktop/backgrounds/xfce-blue.jpg" "$pkg/lib/xfce/"
cp "$desktop/COPYING" "$pkg/lib/xfce/COPYING-xfdesktop"
cp "$desktop/AUTHORS" "$pkg/lib/xfce/AUTHORS-xfdesktop"
cp "$garcon/COPYING" "$pkg/lib/xfce/COPYING-garcon"
cp "$port/applications.menu" "$pkg/lib/xfce/"
cp "$port"/applications/*.desktop "$pkg/lib/xfce/applications/"
# Match the standard time zones offered by LixinOS desktop settings.
for zone in Asia/{Shanghai,Tokyo,Kolkata} Europe/{London,Paris} America/{New_York,Los_Angeles} Australia/Sydney; do
    mkdir -p "$pkg/lib/xfce/zoneinfo/${zone%/*}"
    cp -L "/usr/share/zoneinfo/$zone" "$pkg/lib/xfce/zoneinfo/$zone"
done
source "$port/build-thunar.sh"
mkdir -p "$pkg/lib/xfce/schemas"
cp "$gtk/gtk-3.24.49/gtk/"org.gtk.Settings.*.gschema.xml "$pkg/lib/xfce/schemas/"
glib-compile-schemas --strict "$pkg/lib/xfce/schemas"
echo "Xfce package: $pkg"
