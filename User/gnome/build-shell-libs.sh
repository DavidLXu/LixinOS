#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gnome-port"
prefix="$build/sysroot"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$cache/gtk3-port/tools/bin:${cross[0]}/bin:$PATH"
[[ -f "$build/cross.ini" && -d "$build/p11-kit-0.25.10" ]] || bash "$port/build-deps.sh"
[[ -f "$cache/dbus-port/sdk/lib/libdbus-1.a" ]] || bash "$root/User/dbus/build.sh"

setup() {
    local stage=$1 source=$2
    shift 2
    if [[ -f "$stage/build.ninja" ]]; then
        meson setup --reconfigure "$stage" "$source" "$@"
    else
        meson setup "$stage" "$source" --cross-file "$build/cross.ini" --prefix "$prefix" \
            --libdir lib --default-library static --buildtype minsize --wrap-mode nodownload \
            -Dprefer_static=true "$@"
    fi
}

patch_source() {
    if ! patch -f -Rs --dry-run -p1 -d "$1" < "$port/$2" >/dev/null 2>&1; then
        patch -f -s -p1 -d "$1" < "$port/$2"
    fi
}

jobs=${JOBS:-8}
atspi="$build/atspi-cross"
setup "$atspi" "$build/at-spi2-core-2.58.0" -Datk_only=false -Dintrospection=disabled \
    -Dx11=disabled -Duse_systemd=false -Dgtk2_atk_adaptor=false
meson compile -C "$atspi" -j "$jobs" atk-1.0 atk-bridge-2.0 atspi
meson install --no-rebuild -C "$atspi" --tags devel
pc="$prefix/lib/pkgconfig/atspi-2.pc"
sed '/^Cflags:/s/ -DG_LOG_DOMAIN=.*//' "$pc" > "$pc.tmp"
mv "$pc.tmp" "$pc"

polkit="$build/polkit-cross"
patch_source "$build/polkit-126" polkit-static.patch
setup "$polkit" "$build/polkit-126" -Dlibs-only=true -Dsession_tracking=ConsoleKit \
    -Dauthfw=bsdauth -Dintrospection=false
meson compile -C "$polkit" -j "$jobs"
meson install --no-rebuild -C "$polkit" --tags devel

unpack_sdk_file() {
    local package=$1 filename=$2
    tar -xOf "$build/$package.apk" "usr/$filename" > "$prefix/$filename"
}
unpack_sdk_file libgcrypt-static-1.11.2-r0 lib/libgcrypt.a
unpack_sdk_file libgpg-error-static-1.55-r0 lib/libgpg-error.a
unpack_sdk_file libgcrypt-dev-1.11.2-r0 include/gcrypt.h
unpack_sdk_file libgpg-error-dev-1.55-r0 include/gpg-error.h
unpack_sdk_file libgpg-error-dev-1.55-r0 include/gpgrt.h
for pair in libgcrypt-dev-1.11.2-r0:libgcrypt libgpg-error-dev-1.55-r0:gpg-error; do
    name=${pair#*:}
    tar -xOf "$build/${pair%:*}.apk" "usr/lib/pkgconfig/$name.pc" |
        sed 's|^prefix=.*|prefix=${pcfiledir}/../..|' > "$prefix/lib/pkgconfig/$name.pc"
done

p11="$build/p11-kit-cross"
patch_source "$build/p11-kit-0.25.10" p11-kit-static.patch
setup "$p11" "$build/p11-kit-0.25.10" -Dtest=false -Dnls=false -Dtrust_module=disabled \
    -Dsystemd=disabled -Dbash_completion=disabled -Dzsh_completion=disabled
ninja -C "$p11" -j "$jobs" p11-kit/libp11-kit.a
meson install --no-rebuild -C "$p11" --tags devel

gcr="$build/gcr-cross"
patch_source "$build/gcr-4.4.1" gcr-static.patch
setup "$gcr" "$build/gcr-4.4.1" -Dgtk4=false -Dintrospection=false -Dvapi=false \
    -Dgtk_doc=false -Dssh_agent=false -Dsystemd=disabled -Dgpg_path=/bin/gpg
ninja -C "$gcr" -j "$jobs" gck/libgck-2.a gcr/libgcr-4.a
meson install --no-rebuild -C "$gcr" --tags devel
for name in gck-2 gcr-4; do
    pc="$prefix/lib/pkgconfig/$name.pc"
    sed '/^Cflags:/s/ -DGLIB_VERSION_MIN_REQUIRED=.*//' "$pc" > "$pc.tmp"
    mv "$pc.tmp" "$pc"
done

[[ -f "$prefix/lib/libgtk-4.a" ]] || bash "$port/build-gtk4.sh"
asset_pc="$prefix/share/pkgconfig"
mkdir -p "$asset_pc"
cat > "$asset_pc/xkeyboard-config.pc" <<'PC'
Name: xkeyboard-config
Description: XKB keyboard layouts installed in the GNOME session package
Version: 2.45
xkb_base=/lib/gnome/xkb
PC
cat > "$asset_pc/iso-codes.pc" <<'PC'
prefix=/lib/gnome
Name: iso-codes
Description: ISO language and country databases
Version: 4.19.0
PC
desktop="$build/gnome-desktop-cross"
patch_source "$build/gnome-desktop-44.4" gnome-desktop-base.patch
setup "$desktop" "$build/gnome-desktop-44.4" -Ddesktop_docs=false -Ddebug_tools=false \
    -Dintrospection=false -Dudev=disabled -Dsystemd=disabled -Dlegacy_library=false
meson compile -C "$desktop" -j "$jobs" gnome-desktop-4 gnome-bg-4
meson install --no-rebuild -C "$desktop" --tags devel
