#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
sdk=$(cd "$here/../../Kernel/target/user-build" && pwd)
guest="$sdk/libreoffice/guest"
src="$sdk/libreoffice/libreoffice-25.2.7.2"
if [[ -d /Applications/Xcode.app/Contents/Developer ]]; then
    export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
fi
[[ -f "$guest/config_host.mk" ]] || bash "$here/configure.sh"
cross=("$sdk"/arm-gnu-toolchain-*-aarch64-none-elf/bin)
export PATH="${cross[0]}:$sdk/gtk3-port/tools/bin:/opt/homebrew/opt/make/libexec/gnubin:/opt/homebrew/opt/bison/bin:$PATH"
jobs=${JOBS:-8}
gmake -C "$guest" -j"$jobs" PARALLELISM="$jobs" LDFLAGS= cross-toolset
read -r -a headers <<< "$("$here/pkg-config" --cflags gtk+-3.0)"
"$sdk/gnome-port/cxx" -I"$src/include" "${headers[@]}" -Os -ffunction-sections -fdata-sections \
    -c "$here/window.cpp" -o "$guest/window.o"
export ax_cv_c_float_words_bigendian=no
package=no
if [[ $# == 0 ]]; then
    package=yes
    set -- Package_instsetoo_native_setup Package_instsetoo_native_setup_ure \
        Package_postprocess_registry Rdb_ure/services UnoApi_udkapi UnoApi_offapi UIConfig_modules/swriter \
        Package_extras_cfgusr Package_extras_autotextuser Package_extras_palettes \
        UIConfig_svt UIConfig_vcl UIConfig_sfx UIConfig_svx UIConfig_cui UIConfig_uui \
        UIConfig_filter UIConfig_fps UIConfig_desktop UIConfig_modules/StartModule
    while IFS= read -r target; do set -- "$@" "$target"; done < <(python3 "$here/link.py" --targets)
fi
gmake -C "$guest" -rs -j"$jobs" -f "$src/Makefile.gbuild" BUILDDIR="$guest" \
    CC="$sdk/gnome-port/cc -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -specs=$here/musl.specs" \
    CXX="$sdk/gnome-port/cxx -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -specs=$here/musl.specs" gb_Library_PLAINEXT_FOR_BUILD=.dylib ENABLE_CUSTOMTARGET_COMPONENTS=TRUE "$@"

if [[ $package == yes ]]; then
    python3 "$here/link.py"
    python3 "$here/package.py"
fi
