#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
cache=$(cd "$port/../../Kernel/target/user-build" && pwd)
stage="$cache/gnome-port/mozjs-build"
prefix="$cache/gnome-port/sysroot"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
mkdir -p "$prefix/include/mozjs-140" "$prefix/lib/pkgconfig"
# Mozilla's exported include tree is made of links into the source and build trees.
cp -RL "$stage/dist/include/." "$prefix/include/mozjs-140/"
install -m 644 "$stage/js/src/build/libjs_static.a" "$prefix/lib/libmozjs-140.a"
install -m 644 "$stage/build/pure_virtual/libpure_virtual.a" "$prefix/lib/libmozjs-pure-virtual.a"
install -m 644 "$stage/aarch64-unknown-linux-musl/release/libjsrust.a" "$prefix/lib/libmozjs-rust.a"
# The static shell links MozGlue as individual objects. Export those same objects
# for embedders, excluding the JavaScript console and its editline frontend.
(
    cd "$stage/js/src/shell"
    objects=()
    while IFS= read -r object; do
        case $object in
            ../../../*) objects+=("$object") ;;
        esac
    done < <(tr ' ' '\n' < js.list)
    [[ ${#objects[@]} -gt 0 ]]
    rm -f "$prefix/lib/libmozjs-glue.a"
    "${cross[0]}/bin/aarch64-none-elf-ar" rcsD "$prefix/lib/libmozjs-glue.a" "${objects[@]}"
)
sed -e 's|^prefix=.*|prefix=${pcfiledir}/../..|' \
    -e 's|^exec_prefix=.*|exec_prefix=${prefix}|' \
    -e 's|^libdir=.*|libdir=${exec_prefix}/lib|' \
    -e 's|^includedir=.*|includedir=${prefix}/include|' \
    -e 's|^Cflags:|Cflags: -DXP_UNIX|' \
    -e '/^Libs:/a\
Libs.private: -Wl,--wrap=getenv,--wrap=putenv,--wrap=setenv,--wrap=unsetenv,--wrap=clearenv -lmozjs-pure-virtual -lmozjs-rust -lmozjs-glue -ldl -lm\
Requires.private: zlib\
' "$stage/js/src/build/js.pc" > "$prefix/lib/pkgconfig/mozjs-140.pc"
