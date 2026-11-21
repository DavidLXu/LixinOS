# Sourced by build.sh after the GTK target sysroot is available.
cmake -S "$build/libxml2-2.13.8" -B "$build/libxml2-build" -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_C_COMPILER="$build/cc" -DCMAKE_AR="$(command -v aarch64-none-elf-ar)" \
    -DCMAKE_RANLIB="$(command -v aarch64-none-elf-ranlib)" \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_C_FLAGS="-ffunction-sections -fdata-sections" \
    -DBUILD_SHARED_LIBS=OFF -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_TESTS=OFF \
    -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_MODULES=OFF -DLIBXML2_WITH_ZLIB=OFF
cmake --build "$build/libxml2-build" --parallel "$jobs"
cmake --install "$build/libxml2-build"

gsv="$build/gtksourceview-4.8.4"
sed -i.bak "/^subdir('tests')$/d; /^subdir('testsuite')$/d" "$gsv/meson.build"
sed -i.bak 's/^gtksource_lib = shared_library(/gtksource_lib = library(/' "$gsv/gtksourceview/meson.build"
meson_library sourceview gtksourceview-4.8.4 -Dgir=false -Dvapi=false

mousepad="$build/mousepad-0.6.3"
if ! patch -d "$mousepad" -p1 -R -f --dry-run < "$port/mousepad.patch" >/dev/null 2>&1; then
    patch -d "$mousepad" -p1 < "$port/mousepad.patch"
fi
mousebuild="$build/mousepad-static"
mkdir -p "$mousebuild"
if [[ ! -f "$mousebuild/Makefile" ]]; then
    (
        cd "$mousebuild"
        CC="$build/cc" AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib \
            CFLAGS="-Os -ffunction-sections -fdata-sections" \
            "$mousepad/configure" --host=aarch64-linux-musl --prefix=/lib/gtk3/mousepad \
            --datadir=/lib/gtk3/mousepad --enable-static --disable-shared --disable-nls \
            --enable-keyfile-settings --enable-gtksourceview4 --disable-polkit \
            --disable-plugin-gspell --disable-plugin-shortcuts
    )
fi
make -C "$mousebuild/mousepad" -j"$jobs" libmousepad.la mousepad-main.o
read -r -a libraries <<< "$(pkg-config --libs --static gtksourceview-4 | sed 's/-pthread//g')"
# Pull in the resource object's constructor so GTK can load Mousepad's menus.
"$musl/cc" -nostartfiles -static -s -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
    -Wl,-u,mousepad_get_resource,-T,"$root/User/rust/large.ld" "$build/crt0.o" "$build/crt1.o" \
    "$mousebuild/mousepad/mousepad-main.o" "$mousebuild/mousepad/.libs/libmousepad.a" \
    -Wl,--start-group "${libraries[@]}" "$rt/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group \
    -o "$package/bin/mousepad.win3"
mouse_data="$package/lib/gtk3/mousepad"
schemas="$mouse_data/glib-2.0/schemas"
mkdir -p "$schemas"
cp -R "$prefix/share/gtksourceview-4/"{language-specs,styles} "$mouse_data/"
cp "$gtk/gtk/"org.gtk.Settings.*.gschema.xml "$schemas/"
cp "$mousepad/mousepad/org.xfce.mousepad.gschema.xml" "$schemas/"
glib-compile-schemas --strict "$schemas"
