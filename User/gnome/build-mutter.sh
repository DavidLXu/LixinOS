#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gnome-port"
gtk="$cache/gtk3-port"
wl="$cache/wayland-port/sdk"
prefix="$build/sysroot"
src="$build/mutter-49.0"
stage="$build/mutter-cross"

if [[ ! -f "$prefix/lib/libcolord.a" || ! -f "$prefix/lib/libdisplay-info.a" || ! -f "$prefix/lib/libharfbuzz.a" ]]; then
    bash "$port/build-deps.sh"
fi
[[ -f "$prefix/lib/libgbm.a" ]] || bash "$port/build-mesa.sh"
[[ -f "$prefix/lib/libinput.a" ]] || bash "$port/build-input.sh"
[[ -f "$cache/dbus-port/sdk/lib/libdbus-1.a" ]] || bash "$root/User/dbus/build.sh"
[[ -f "$wl/include/lixin-wayland-loop.h" ]] || bash "$root/User/wayland/build.sh"
[[ -f "$build/xkb-data/share/xkeyboard-config-2/rules/evdev" ]] || bash "$port/build-xkb-data.sh"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$wl/bin:$gtk/tools/bin:${cross[0]}/bin:$PATH"
export PKG_CONFIG_PATH="$wl/lib/pkgconfig"
if [[ ! -x "$build/tools/bin/cvt" ]]; then
    [[ -d "$build/libxcvt-0.1.3" ]] || bash "$port/build-deps.sh"
    meson setup "$build/cvt-native" "$build/libxcvt-0.1.3" --prefix "$build/tools" \
        --default-library static --buildtype release
    meson compile -C "$build/cvt-native"
    meson install --no-rebuild -C "$build/cvt-native"
fi
export PATH="$build/tools/bin:$PATH"
for diff in "$port/mutter-wayland.patch" "$port/mutter-pixbuf.patch" "$port/mutter-headless.patch" \
    "$port/mutter-static-plugin.patch" "$port/mutter-static-gvdb.patch" "$port/mutter-display.patch"; do
    if ! patch -f -Rs --dry-run -p1 -d "$src" < "$diff" >/dev/null 2>&1; then
        patch -f -s -p1 -d "$src" < "$diff"
    fi
done
install -m 644 "$port/wayland-source.c" "$port/wayland-source.h" "$src/src/wayland/"
install -m 644 "$port/mutter-display.c" "$src/src/backends/lixin-display.c"
install -m 644 "$port/mutter-display.h" "$src/src/backends/lixin-display.h"
install -m 644 "$root/User/gui/lixin-gui.h" "$src/src/"

options=(-Dwayland=true -Dx11=false -Dxwayland=false -Dopengl=false -Dglx=false -Dgles2=true
         -Dprefer_static=true -Dnative_backend=true -Dudev=true -Dlogind=false -Dlibgnome_desktop=false
         -Dstartup_notification=false -Dsound_player=false -Dlibwacom=false -Dremote_desktop=false
         -Dintrospection=false -Dprofiler=false -Ddocs=false -Ddevkit=disabled -Dtests=disabled
         -Dbash_completion=false)
if [[ -f "$stage/build.ninja" ]]; then
    meson setup --reconfigure "$stage" "$src" "${options[@]}"
else
    meson setup "$stage" "$src" --cross-file "$build/cross.ini" --prefix "$prefix" --libdir lib \
        --default-library static --buildtype minsize --wrap-mode nodownload "${options[@]}"
fi
meson compile -C "$stage" -j "${JOBS:-8}" mutter
meson install --no-rebuild -C "$stage" --tags devel
