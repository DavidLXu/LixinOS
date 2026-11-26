#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gnome-port"
prefix="$build/sysroot"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$cache/wayland-port/sdk/bin:$cache/gtk3-port/tools/bin:${cross[0]}/bin:$PATH"
jobs=${JOBS:-8}

if [[ ! -f "$build/cross.ini" || ! -d "$build/geoclue-2.7.2" ]]; then
    bash "$port/build-deps.sh"
fi

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
    local source=$1 file=$2
    if ! patch -f -Rs --dry-run -p1 -d "$source" < "$port/$file" >/dev/null 2>&1; then
        patch -f -s -p1 -d "$source" < "$port/$file"
    fi
}

geoclue() {
    local source="$build/geoclue-2.7.2" stage="$build/geoclue-cross"
    patch_source "$source" geoclue-static.patch
    setup "$stage" "$source" -Denable-backend=false -Ddemo-agent=false \
        -Dintrospection=false -Dvapi=false -Dgtk-doc=false
    meson compile -C "$stage" -j "$jobs" geoclue-2
    meson install -C "$stage" --no-rebuild --tags devel
}

upower() {
    local source="$build/upower-v1.90.9" stage="$build/upower-cross"
    patch_source "$source" upower-static.patch
    setup "$stage" "$source" -Dos_backend=dummy -Didevice=disabled -Dpolkit=disabled \
        -Dgtk-doc=false -Dman=false -Dintrospection=disabled -Dzshcompletiondir=no \
        -Dsystemdsystemunitdir=no
    meson compile -C "$stage" -j "$jobs" upower-glib
    meson install -C "$stage" --no-rebuild --tags devel
}

ibus() {
    local stage="$build/ibus-cross"
    # On an existing build the daemon patch already includes the client change.
    if ! patch -f -Rs --dry-run -p1 -d "$build/ibus-1.5.35" < "$port/ibus-daemon.patch" >/dev/null 2>&1; then
        patch_source "$build/ibus-1.5.35" ibus-client.patch
        patch_source "$build/ibus-1.5.35" ibus-daemon.patch
    fi
    patch_source "$build/ibus-1.5.35" ibus-parent.patch
    setup "$stage" "$build/ibus-1.5.35" -Dtests=false -Dxim=disabled -Dwayland=disabled \
        -Dgtk2=disabled -Dgtk3=false -Dgtk4=false -Dui=false -Dengine=false -Dsetup=false \
        -Dlibnotify=false -Dappindicator=false -Demoji-dict=false -Dunicode-dict=false \
        -Dsystemd-services=disabled -Ddconf=disabled -Dintrospection=false -Dvapi=disabled -Dgtk-doc=false
    meson compile -C "$stage" -j "$jobs" ibus-1.0 ibus-daemon
    meson install -C "$stage" --no-rebuild --tags devel
}

accounts() {
    local source="$build/accountsservice-23.13.9" stage="$build/accounts-cross"
    patch_source "$source" accounts-client.patch
    setup "$stage" "$source" -Dintrospection=false -Dvapi=false
    meson compile -C "$stage" -j "$jobs" accountsservice
    meson install -C "$stage" --no-rebuild --tags devel
}

gdm() {
    local source="$build/gdm-49.0" stage="$build/gdm-cross"
    patch_source "$source" gdm-client.patch
    setup "$stage" "$source"
    meson compile -C "$stage" -j "$jobs" gdm
    meson install -C "$stage" --no-rebuild --tags devel
}

network() {
    bash "$port/fetch-shell-sdk.sh"
    local pkg path pc stage
    [[ -d "$build/libpsl-0.21.5" ]] || bash "$port/build-deps.sh"
    for pkg in libidn2-static-2.3.8-r0 libunistring-static-1.4.1-r0 sqlite-static-3.53.4-r0; do
        while IFS= read -r path; do
            tar -xOf "$build/$pkg.apk" "$path" > "$prefix/${path#usr/}"
        done < <(tar -tf "$build/$pkg.apk" | awk '/^usr\/lib\/.*\.a$/')
    done
    for pkg in libidn2-dev-2.3.8-r0 libunistring-dev-1.4.1-r0 nghttp2-dev-1.69.0-r0 sqlite-dev-3.53.4-r0; do
        while IFS= read -r path; do
            path=${path#usr/}
            mkdir -p "$prefix/$(dirname "$path")"
            tar -xOf "$build/$pkg.apk" "usr/$path" > "$prefix/$path"
        done < <(tar -tf "$build/$pkg.apk" | awk '/^usr\/include\// && substr($0, length($0)) != "/"')
        while IFS= read -r path; do
            pc="$prefix/lib/pkgconfig/${path##*/}"
            tar -xOf "$build/$pkg.apk" "$path" |
                sed -e 's|^prefix=.*|prefix=${pcfiledir}/../..|' -e 's| -L/usr/lib||g' > "$pc"
        done < <(tar -tf "$build/$pkg.apk" | awk '/^usr\/lib\/pkgconfig\/.*\.pc$/')
    done
    # Alpine's nghttp2 archive has native code and GCC LTO bytecode.
    # Keep the AArch64 objects independent of the cross compiler's LTO version.
    tar -xOf "$build/nghttp2-static-1.69.0-r0.apk" usr/lib/libnghttp2.a > "$build/libnghttp2-fat.a"
    aarch64-none-elf-objcopy --wildcard '--remove-section=.gnu.lto_*' \
        "$build/libnghttp2-fat.a" "$prefix/lib/libnghttp2.a"
    aarch64-none-elf-ranlib "$prefix/lib/libnghttp2.a"

    stage="$build/psl-cross"
    patch_source "$build/libpsl-0.21.5" psl-static.patch
    setup "$stage" "$build/libpsl-0.21.5" -Druntime=libidn2 -Dtests=false -Ddocs=false
    ninja -C "$stage" -j "$jobs" src/libpsl.a
    meson install -C "$stage" --no-rebuild --tags devel

    stage="$build/soup-cross"
    setup "$stage" "$build/libsoup-3.6.5" -Dtests=false -Ddocs=disabled -Dintrospection=disabled \
        -Dvapi=disabled -Dgssapi=disabled -Dntlm=disabled -Dbrotli=disabled \
        -Dsysprof=disabled -Dpkcs11_tests=disabled -Dtls_check=false
    meson compile -C "$stage" -j "$jobs" soup-3.0
    meson install -C "$stage" --no-rebuild --tags devel

    stage="$build/geocode-cross"
    patch_source "$build/geocode-glib-3.26.4" geocode-static.patch
    setup "$stage" "$build/geocode-glib-3.26.4" -Dsoup2=false -Denable-installed-tests=false \
        -Denable-introspection=false -Denable-gtk-doc=false
    meson compile -C "$stage" -j "$jobs" geocode-glib-2
    meson install -C "$stage" --no-rebuild --tags devel

    stage="$build/gweather-cross"
    patch_source "$build/libgweather-4.4.4" gweather-static.patch
    setup "$stage" "$build/libgweather-4.4.4" -Dtests=false -Dgtk_doc=false \
        -Denable_vala=false -Dintrospection=false
    ninja -C "$stage" -j "$jobs" libgweather/libgweather-4.a schemas/org.gnome.GWeather4.enums.xml
    meson install -C "$stage" --no-rebuild --tags devel
}

if (( $# == 0 )); then set -- geoclue upower ibus accounts gdm network; fi
for client do
    case "$client" in
        geoclue|upower|ibus|accounts|gdm|network) "$client" ;;
        *) printf 'Unknown GNOME client library %s\n' "$client" >&2; exit 2 ;;
    esac
done
