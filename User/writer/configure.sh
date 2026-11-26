#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
sdk="$root/Kernel/target/user-build"
work="$sdk/libreoffice"
version=25.2.7.2
src="$work/libreoffice-$version"
mkdir -p "$work/guest"
[[ -f "$sdk/gtk3-port/sysroot/lib/pkgconfig/gtk+-3.0.pc" ]] || bash "$root/User/gtk3/build.sh"
[[ -x "$sdk/gnome-port/cxx" ]] || bash "$root/User/gnome/build-deps.sh"
if [[ ! -f "$work/core.tar.xz" ]]; then
    curl -fLsS --retry 3 "https://downloadarchive.documentfoundation.org/libreoffice/old/$version/src/libreoffice-$version.tar.xz" -o "$work/core.tar.xz.tmp"
    mv "$work/core.tar.xz.tmp" "$work/core.tar.xz"
fi
(cd "$work" && shasum -a 256 -c "$root/User/writer/source.sha256")
[[ -f "$src/sw/Library_sw.mk" ]] || tar xf "$work/core.tar.xz" -C "$work"
if ! grep -q '$(AR) rcs libargon2.a' "$src/external/argon2/ExternalProject_argon2.mk"; then
    patch -d "$src" -p1 < "$root/User/writer/argon2-static.patch"
fi
if ! grep -q 'extern int writer_window' "$src/desktop/source/app/main.c"; then
    patch -d "$src" -p1 < "$root/User/writer/window-entry.patch"
fi
if ! grep -q 'extern __attribute__((__weak__))' "$src/idl/source/objects/types.cxx"; then
    patch -d "$src" -p1 < "$root/User/writer/svidl-static.patch"
fi
# musl uses the POSIX strerror_r signature even when _GNU_SOURCE is enabled.
cp "$root/User/writer/coin-null.patch.0" "$src/external/coinmp/"
if ! grep -q 'coin-null.patch.0' "$src/external/coinmp/UnpackedTarball_coinmp.mk"; then
    printf '\n%s\n' '$(eval $(call gb_UnpackedTarball_add_patches,coinmp,external/coinmp/coin-null.patch.0))' >> "$src/external/coinmp/UnpackedTarball_coinmp.mk"
fi
patchdir="$src/external/openssl"
cp "$root/User/writer/openssl-musl.patch.0" "$patchdir/"
if ! grep -q 'openssl-musl.patch.0' "$patchdir/UnpackedTarball_openssl.mk"; then
    printf '\n%s\n' '$(eval $(call gb_UnpackedTarball_add_patches,openssl,external/openssl/openssl-musl.patch.0))' >> "$patchdir/UnpackedTarball_openssl.mk"
fi
if ! grep -q '__lixinos__' "$src/cppuhelper/source/paths.cxx"; then
    patch -d "$src" -p1 < "$root/User/writer/static-prefix.patch"
fi
if ! grep -q 'lx_writer_map_readonly' "$src/sal/osl/unx/file.cxx"; then
    patch -d "$src" -p1 < "$root/User/writer/readonly-map.patch"
fi
if ! grep -q 'caller retains the handle' "$src/store/source/lockbyte.cxx"; then
    patch -d "$src" -p1 < "$root/User/writer/mapping-ownership.patch"
fi
if ! grep -q '__lixinos__' "$src/sal/osl/unx/random.cxx"; then
    patch -d "$src" -p1 < "$root/User/writer/entropy.patch"
fi
if ! grep -q lx_writer_uno_arguments "$src/bridges/source/cpp_uno/gcc3_linux_aarch64/cpp2uno.cxx"; then
    patch -d "$src" -p1 < "$root/User/writer/uno-static.patch"
fi
if ! grep -q guestFileTransfer "$src/sal/osl/unx/file.cxx"; then
    patch -d "$src" -p1 < "$root/User/writer/file-transfer.patch"
fi
if ! grep -q '!defined __EMSCRIPTEN__ && !defined __lixinos__' "$src/desktop/source/lib/init.cxx"; then
    patch -d "$src" -p1 < "$root/User/writer/optional-signing.patch"
fi
if ! grep -q lx_writer_map_readonly "$src/vcl/unx/generic/glyphs/freetype_glyphcache.cxx"; then
    patch -d "$src" -p1 < "$root/User/writer/fonts.patch"
fi
cross=("$sdk"/arm-gnu-toolchain-*-aarch64-none-elf/bin)
export PATH="${cross[0]}:$sdk/gtk3-port/tools/bin:/opt/homebrew/opt/make/libexec/gnubin:/opt/homebrew/opt/bison/bin:$PATH"
export CC="$sdk/gnome-port/cc" CXX="$sdk/gnome-port/cxx"
export AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib
export PKG_CONFIG="$root/User/writer/pkg-config"
export CFLAGS="-Os -D_GNU_SOURCE" CXXFLAGS="-Os -D_GNU_SOURCE" LDFLAGS=-nodefaultlibs
if [[ -d /Applications/Xcode.app/Contents/Developer ]]; then
    export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
fi
export NM=aarch64-none-elf-nm STRIP=aarch64-none-elf-strip READELF=aarch64-none-elf-readelf
cd "$work/guest"
cp "$src/configure" ./configure
chmod +x ./configure
exec bash ./configure --srcdir="$src" --build="$(sh "$src/config.guess")" \
    --with-build-platform-configure-options="LDFLAGS= CXXFLAGS= PKG_CONFIG=$(command -v pkg-config) --disable-ccache --enable-bogus-pkg-config" --host=aarch64-unknown-linux-musl --disable-gui --disable-dynamic-loading --enable-customtarget-components \
    --without-system-libxml --without-system-icu --with-galleries=no --disable-database-connectivity --without-fonts --with-main-module=writer --without-java --disable-python --disable-skia \
    --disable-cups --disable-dbus --disable-gstreamer-1-0 --disable-gtk3 \
    --disable-firebird-sdbc --disable-postgresql-sdbc --disable-odk --without-help \
    --without-gssapi --without-krb5 --disable-mariadb-sdbc --disable-nss --disable-extensions --disable-scripting --without-doxygen --disable-online-update --disable-crashdump --disable-pdfium
