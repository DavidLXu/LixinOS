#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
cache=$(cd "$port/../../Kernel/target/user-build" && pwd)
build="$cache/gnome-port"
src="$build/libffi-3.4.8"
stage="$build/ffi-cross"
prefix="$build/sysroot"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
[[ -d "$src" ]] || tar -xf "$cache/gtk3-port/libffi.tar.gz" -C "$build"
install -m 644 "$port/lixin-ffi.h" "$port/lixin-ffi-closures.c" "$src/src/"
install -m 644 "$port/lixin-ffi-trampolines.S" "$src/src/aarch64/"
if ! patch -f -Rs --dry-run -p1 -d "$src" < "$port/ffi-closures.patch" >/dev/null 2>&1; then
    patch -f -s -p1 -d "$src" < "$port/ffi-closures.patch"
fi
mkdir -p "$stage"
cd "$stage"
if [[ ! -f Makefile ]]; then
    CC="$build/cc" AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib \
        "$src/configure" --host=aarch64-linux-musl --prefix="$prefix" --libdir="$prefix/lib" \
        --disable-shared --disable-docs --disable-exec-static-tramp
fi
make -j "${JOBS:-8}" install
