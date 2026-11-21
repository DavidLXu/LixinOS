# Sourced by build-games.sh before Mines is compiled.
while read -r dependency series release hash; do
    archive_dep="$extra/$dependency-$release.tar.xz"
    if [[ ! -f "$archive_dep" ]]; then
        curl -fLsS --retry 3 "https://download.gnome.org/sources/$dependency/$series/$dependency-$release.tar.xz" \
            -o "$archive_dep.tmp"
        printf '%s  %s\n' "$hash" "$archive_dep.tmp" | shasum -a 256 -c -
        mv "$archive_dep.tmp" "$archive_dep"
    fi
    printf '%s  %s\n' "$hash" "$archive_dep" | shasum -a 256 -c -
    [[ -d "$extra/$dependency-$release" ]] || tar -xf "$archive_dep" -C "$extra"
done < "$port/gnome-games-deps.tsv"

gee="$extra/libgee-0.20.8"
gee_build="$extra/gee-build"
mkdir -p "$gee_build"
if [[ ! -f "$gee_build/Makefile" ]]; then
    (
        cd "$gee_build"
        CC="$build/cc" AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib \
            CFLAGS="-Os -ffunction-sections -fdata-sections" \
            "$gee/configure" --host=aarch64-linux-musl --prefix="$sysroot" \
            --enable-static --disable-shared --disable-doc --enable-introspection=no
    )
fi
make -C "$gee_build/gee" -j"${JOBS:-8}" install
cp "$gee_build/gee-0.8.pc" "$sysroot/lib/pkgconfig/"

support="$extra/libgnome-games-support-1.8.2"
support_build="$extra/games-support-generated"
mkdir -p "$support_build"
(
    cd "$support/games"
    valac -C -d "$support_build" --vapidir "$gee/gee" \
        --pkg gee-0.8 --pkg gtk+-3.0 --pkg gio-2.0 --target-glib=2.44 \
        --header "$support_build/libgnome-games-support.h" \
        --library libgnome-games-support-1 --vapi "$support_build/libgnome-games-support-1.vapi" \
        config.vapi gridframe.vala scores/*.vala
)
printf '#define GETTEXT_PACKAGE "libgnome-games-support"\n#define LOCALEDIR "/lib/mines/locale"\n' > "$support_build/config.h"
read -r -a support_flags <<< "$(pkg-config --cflags --static gtk+-3.0 gee-0.8 | sed 's/-pthread//g')"
support_objects=()
for source in "$support_build"/*.c "$support_build"/scores/*.c; do
    [[ -f "$source" ]] || continue
    object="$support_build/${source##*/}.o"
    "$musl/cc" -Os -ffunction-sections -fdata-sections "${support_flags[@]}" \
        -I"$support_build" -include "$support_build/config.h" -c "$source" -o "$object"
    support_objects+=("$object")
done
aarch64-none-elf-ar rcs "$support_build/libgnome-games-support.a" "${support_objects[@]}"
