#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gnome-port"
prefix="$build/sysroot"
src="$build/gjs-1.86.0"
stage="$build/gjs-cross"
work="$build/gjs-native-gi"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$cache/gtk3-port/tools/bin:${cross[0]}/bin:$PATH"
mkdir -p "$work/gir" "$work/typelib"

fetch_glib_metadata() {
    local file=$1 checksum=$2
    if [[ ! -s "$build/$file" ]]; then
        curl -fLsS --retry 3 "https://dl-cdn.alpinelinux.org/alpine/v3.23/main/aarch64/$file" \
            -o "$build/$file.tmp"
        printf '%s  %s\n' "$checksum" "$build/$file.tmp" | shasum -a 256 -c -
        mv "$build/$file.tmp" "$build/$file"
    fi
    printf '%s  %s\n' "$checksum" "$build/$file" | shasum -a 256 -c -
}
fetch_glib_metadata glib-dev-2.86.5-r0.apk 898a566960ea7e02771f89f6b4c399e0dc2b78a33ef7fc82ea33b2d4ff9b940a
fetch_glib_metadata glib-2.86.5-r0.apk 39bfc3bf9da7c0030238f0bc6201d36903ea7dda55ec6be9465115e200bd9074
for name in GLib-2.0 GObject-2.0 Gio-2.0 GLibUnix-2.0 GioUnix-2.0 GModule-2.0 GIRepository-3.0; do
    tar -xOf "$build/glib-dev-2.86.5-r0.apk" "usr/share/gir-1.0/$name.gir" > "$work/gir/$name.gir"
    tar -xOf "$build/glib-2.86.5-r0.apk" "usr/lib/girepository-1.0/$name.typelib" > "$work/typelib/$name.typelib"
done

# The scanner executes a host library built from GJS's own private sources to
# get GType information. The guest only needs the compiled metadata.
(
    cd "$work"
    printf '#define HAVE_UNISTD_H 1\n' > config.h
    IFS=' ' read -r -a cflags <<< "$(pkg-config --cflags gobject-2.0 gio-2.0 girepository-2.0)"
    IFS=' ' read -r -a libs <<< "$(pkg-config --libs gobject-2.0 gio-2.0 girepository-2.0)"
    c++ -std=c++17 -DXP_UNIX -DXP_DARWIN -I"$work" -I"$src" -I"$prefix/include/mozjs-140" \
        "${cflags[@]}" -c "$src/util/console.cpp" -o console.o
    cc -dynamiclib -I"$work" -I"$src" "${cflags[@]}" "$src"/libgjs-private/*.c console.o \
        "${libs[@]}" -o libgjs-gir.dylib
    DYLD_LIBRARY_PATH="$work" g-ir-scanner --no-libtool --namespace=GjsPrivate --nsversion=1.0 \
        --identifier-prefix=Gjs --symbol-prefix=gjs --include=GObject-2.0 --include=Gio-2.0 \
        --pkg=gio-2.0 --pkg=girepository-2.0 --library=gjs-gir -L"$work" -I"$src" -I"$work" \
        -o GjsPrivate-1.0.gir "$src"/libgjs-private/*.[ch]
    sed 's/libgjs-gir\.dylib/libgjs.so.0/' GjsPrivate-1.0.gir > gir/GjsPrivate-1.0.gir
    gi-compile-repository --includedir=gir gir/GjsPrivate-1.0.gir -o typelib/GjsPrivate-1.0.typelib
)

args=()
for gir in "$work/gir"/*.gir; do args+=(--gir "$gir"); done
for name in glib-2.0 gobject-2.0 gio-2.0 gmodule-2.0 girepository-2.0 gjs; do args+=(--archive "$prefix/lib/lib$name.a"); done
python3 "$port/gen-static-modules.py" --nm aarch64-none-elf-nm "${args[@]}" --output "$work/modules.c"
"$build/cc" -Os -ffunction-sections -fdata-sections -I"$port" -c "$work/modules.c" -o "$work/modules.o"
python3 - "$stage" "$work/modules.o" <<'PY'
from pathlib import Path
import shlex
import subprocess
import sys

stage, registry = sys.argv[1:]
line = subprocess.check_output(["ninja", "-t", "commands", "gjs-console"], cwd=stage, text=True).splitlines()[-1]
command = shlex.split(line)
output = command.index("-o") + 1
assert command[output] == "gjs-console"
command[output] = "gjs-lixinos"
command.insert(output + 1, registry)
subprocess.run(command, cwd=Path(stage), check=True)
PY
