#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../../.." && pwd)
sdk="$root/Kernel/target/user-build"
gtk="$sdk/gtk3-port"
b="$sdk/dsh-gui"
if [[ ! -f "$gtk/sysroot/lib/libgtk-3.a" || "$root/User/gtk3/gdk-lixinos-server.c" -nt "$gtk/sysroot/lib/libgdk-3.a" ]]; then
    bash "$root/User/gtk3/build.sh"
fi
cross=("$sdk"/arm-gnu-toolchain-*/bin)
export PATH="$gtk/tools/bin:${cross[0]}:$PATH" PKG_CONFIG_LIBDIR="$gtk/sysroot/lib/pkgconfig" PKG_CONFIG_PATH=
mkdir -p "$b"
md4c="$b/md4c-release-0.5.2"
if [[ ! -f "$md4c/src/md4c.c" ]]; then
    archive="$b/md4c-0.5.2.tar.gz"
    [[ -f "$archive" ]] || curl -fLsS --retry 2 https://codeload.github.com/mity/md4c/tar.gz/refs/tags/release-0.5.2 -o "$archive"
    echo "55d0111d48fb11883aaee91465e642b8b640775a4d6993c2d0e7a8092758ef21  $archive" | shasum -a 256 -c -
    tar xzf "$archive" -C "$b"
fi
if [[ ! -f "$gtk/sysroot/lib/libjson-glib-1.0.a" ]]; then
    src=json-glib-1.8.0
    curl -fLsS --retry 2 "https://download.gnome.org/sources/json-glib/1.8/$src.tar.xz" -o "$b/$src.tar.xz"
    echo "97ef5eb92ca811039ad50a65f06633f1aae64792789307be7170795d8b319454  $b/$src.tar.xz" | shasum -a 256 -c -
    tar xf "$b/$src.tar.xz" -C "$b"
    meson setup "$b/json-build" "$b/$src" --cross-file "$gtk/cross.ini" --prefix "$gtk/sysroot" \
        --libdir lib --buildtype minsize --default-library static --wrap-mode nodownload \
        -Dintrospection=disabled -Dman=false -Dtests=false -Dgtk_doc=disabled
    meson install -C "$b/json-build"
fi
read -r -a flags <<< "$(pkg-config --cflags gtk+-3.0 json-glib-1.0 | sed 's/-pthread//g')"
read -r -a libs <<< "$(pkg-config --libs --static gtk+-3.0 json-glib-1.0 | sed 's/-pthread//g')"
"$gtk/runtime/cc" -Os -Wall -ffunction-sections -fdata-sections "${flags[@]}" \
    -I"$sdk/cpython-port/musl-1.2.5/src/process" -I"$md4c/src" "$port/gui.c" "$port/engine.c" \
    "$port/markdown.c" "$md4c/src/md4c.c" "$md4c/src/entity.c" \
    "$root/User/rust/spawn.c" -Wl,--start-group "${libs[@]}" -Wl,--end-group -o "$b/dsh-gui"
stage=$(mktemp -d "$b/stage.XXXXXX")
trap 'rm -rf "$stage"' EXIT
python3 - "$root" "$b" "$stage" <<'PY'
from pathlib import Path
import sys, runpy
root, build, stage = map(Path, sys.argv[1:])
pack = runpy.run_path(str(root / "User/gcc/package.py"))
pack["tool"](stage, "/bin/dsh-gui.win3", build / "dsh-gui", library="/lib/dsh-gui",
             launcher_flag="-DLX_DBUS", build_dir=build, deflate=True)
PY
rsvg-convert -w 42 -h 42 "$port/deepseek.svg" -o "$stage/lib/dsh-gui/deepseek.png"
cp "$port/LICENSE-deepseek" "$port/control.mjs" "$port/control.yml" "$stage/lib/dsh-gui/"
cp "$md4c/LICENSE.md" "$stage/lib/dsh-gui/LICENSE-md4c"
mkdir -p "$b/package"
rsync -a --delete "$stage/" "$b/package/"
