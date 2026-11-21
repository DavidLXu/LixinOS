#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gtk3-port"
musl="$cache/cpython-port"
sysroot="$build/sysroot"
extra="$build/extra"
app=${1:-tetravex}
series=3.38
packages=(gtk+-3.0)
extra_link=()
extra_include=()
case "$app" in
    tetravex)
        version=3.38.3
        checksum=83849ac064d456e1dd46b6e201c686152624c3d1ec4b99935985a49aa79172cb
        id=org.gnome.Tetravex
        gettext=gnome-tetravex-gui
        sources=(cli config.vapi history puzzle puzzle-view score-dialog score-overlay
                 theme-extrusion theme-neoretro theme-nostalgia theme-synesthesia)
        options=(--enable-experimental-non-null)
        resource_path=src/gnome-tetravex.gresource.xml
        ;;
    mahjongg)
        version=3.38.3
        checksum=d2b1e47d85852048b35d89e3ddaba1daeb06aaf97acfb67a501ce664ff509190
        id=org.gnome.Mahjongg
        gettext=gnome-mahjongg
        sources=(config.vapi game game-view history map score-dialog)
        options=()
        resource_path=data/org.gnome.Mahjongg.gresource.xml
        ;;
    mines)
        series=40
        version=40.1
        checksum=3502e9b3f71cb3b2e711c0e6019187febcc2bca877e3d456dcab700f7be31272
        id=org.gnome.Mines
        gettext=gnome-mines
        sources=(config.vapi minefield minefield-view tile theme-selector-dialog)
        options=()
        resource_path=src/gnome-mines.gresource.xml
        ;;
    *) echo "Usage: $0 [tetravex|mahjongg|mines]" >&2; exit 2 ;;
esac
name="gnome-$app-$version"
archive="$extra/$name.tar.xz"
package="$extra/$app-package"
stage="$extra/$app-generated"
app_dir="/lib/$app"

[[ -f "$sysroot/lib/pkgconfig/gtk+-3.0.pc" ]] || bash "$port/build.sh"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
for tool in valac glib-compile-resources glib-compile-schemas; do
    command -v "$tool" >/dev/null || { echo "Required host tool: $tool" >&2; exit 1; }
done
mkdir -p "$extra" "$stage" "$package/bin" "$package$app_dir/schemas"
if [[ ! -f "$archive" ]]; then
    curl -fLsS --retry 3 "https://download.gnome.org/sources/gnome-$app/$series/$name.tar.xz" -o "$archive.tmp"
    printf '%s  %s\n' "$checksum" "$archive.tmp" | shasum -a 256 -c -
    mv "$archive.tmp" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | shasum -a 256 -c -
[[ -d "$extra/$name" ]] || tar -xf "$archive" -C "$extra"

upstream="$extra/$name"
export PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR="$sysroot/lib/pkgconfig"
if [[ "$app" == mines ]]; then
    source "$port/build-mines-deps.sh"
    packages+=(gee-0.8)
    options=(--vapidir "$support_build" --vapidir "$gee/gee" --pkg libgnome-games-support-1 --pkg gee-0.8)
    extra_link+=("$support_build/libgnome-games-support.a")
    extra_include+=("-I$support_build")
fi
main="$stage/gnome-$app.vala"
python3 "$port/prepare-game.py" "$app" "$upstream" "$stage"
resource="$upstream/$resource_path"
inputs=()
for source in "${sources[@]}"; do
    target="$stage/$source.vala"
    if [[ -f "$target" ]]; then
        inputs+=("$target")
    elif [[ "$source" == *.vapi ]]; then
        inputs+=("$upstream/src/$source")
    else
        inputs+=("$upstream/src/$source.vala")
    fi
done
(
    cd "$stage"
    env -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_PATH valac -C -d "$stage" \
        --pkg gtk+-3.0 --pkg gio-2.0 --pkg posix ${options[@]+"${options[@]}"} \
        --gresources "$resource" --gresourcesdir "$(dirname "$resource")" \
        "${inputs[@]}" "$main"
    glib-compile-resources "$resource" --sourcedir "$(dirname "$resource")" \
        --c-name resources --generate-source --target "$stage/resources.c"
)
read -r -a flags <<< "$(pkg-config --cflags --static "${packages[@]}" | sed 's/-pthread//g')"
read -r -a libraries <<< "$(pkg-config --libs --static "${packages[@]}" | sed 's/-pthread//g')"
"$musl/cc" -Os -ffunction-sections -fdata-sections "${flags[@]}" \
    ${extra_include[@]+"${extra_include[@]}"} \
    -DVERSION="\"$version\"" -DGETTEXT_PACKAGE="\"$gettext\"" \
    -DLOCALEDIR="\"$app_dir/locale\"" -DDATA_DIRECTORY="\"$app_dir\"" "$stage"/*.c \
    -nostartfiles -static -s -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
    -Wl,-T,"$root/User/rust/large.ld" "$build/crt0.o" "$build/crt1.o" \
    -Wl,--start-group ${extra_link[@]+"${extra_link[@]}"} "${libraries[@]}" "$build/runtime/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group \
    -o "$package/bin/$app.win3"
cp "$upstream/COPYING" "$package$app_dir/"
schema="$upstream/data/$id.gschema.xml"
if [[ "$app" == mahjongg ]]; then
    mkdir -p "$package$app_dir"/{maps,themes}
    cp "$upstream/data/mahjongg.map" "$package$app_dir/maps/"
    cp "$upstream/data"/{smooth.png,edu_kang_xi.png} "$package$app_dir/themes/"
    rsvg-convert --zoom 2 "$upstream/data/postmodern.svg" -o "$package$app_dir/themes/postmodern.png"
    sed 's/postmodern\.svg/postmodern.png/' "$schema" > "$package$app_dir/schemas/$id.gschema.xml"
elif [[ "$app" == mines ]]; then
    for theme in "$upstream/data/themes/"*; do
        dest="$package$app_dir/themes/${theme##*/}"
        mkdir -p "$dest"
        sed 's/\.svg/.png/g' "$theme/theme.css" > "$dest/theme.css"
        for icon in "$theme"/*.svg; do
            name_png=${icon##*/}
            rsvg-convert --width 96 --height 96 "$icon" -o "$dest/${name_png%.svg}.png"
        done
    done
    cp "$gee/COPYING" "$package$app_dir/COPYING-libgee"
    cp "$support/COPYING" "$package$app_dir/COPYING-libgnome-games-support"
    cp "$schema" "$package$app_dir/schemas/"
else
    cp "$schema" "$package$app_dir/schemas/"
fi
glib-compile-schemas --strict "$package$app_dir/schemas"
echo "$app package: $package"
