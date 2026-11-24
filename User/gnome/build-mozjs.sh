#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/gnome-port"
src="$build/firefox-140.0"
stage="$build/mozjs-build"
digest=12e40b81627ac4b44d43edb94aa1cd2a6c50bb49503e3b1b6f80259eed197ecb5847a0f0520ad204340dfe0ab7b5a4aa1ced2bc21ee0258471e8fdb121f6aaf4

[[ -f "$build/runtime/rt/liblx.a" ]] || bash "$port/build-deps.sh"
LIXINOS_GCC_BUILD="$build/runtime" bash "$root/User/gcc/build.sh" rt
mkdir -p "$build" "$stage"
archive="$build/firefox-140.0esr.source.tar.xz"
if [[ ! -s "$archive" ]]; then
    curl -fLsS --retry 3 "https://archive.mozilla.org/pub/firefox/releases/140.0esr/source/${archive##*/}" -o "$archive.tmp"
    printf '%s  %s\n' "$digest" "$archive.tmp" | shasum -a 512 -c -
    mv "$archive.tmp" "$archive"
fi
printf '%s  %s\n' "$digest" "$archive" | shasum -a 512 -c -
if [[ ! -f "$src/configure.py" ]]; then
    unpack=$(mktemp -d "$build/moz-src.XXXXXX")
    trap 'rm -rf "$unpack"' EXIT
    tar -xf "$archive" -C "$unpack"
    mv "$unpack/firefox-140.0" "$src"
    rmdir "$unpack"
    trap - EXIT
fi

cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
llvm=${LIXINOS_LLVM_BIN:-/opt/homebrew/opt/llvm@22/bin}
export LIXINOS_LLVM_BIN="$llvm"
export PATH="$build/moz-tools/bin:$HOME/.cargo/bin:$llvm:${cross[0]}/bin:$PATH"
if [[ ! -x "$build/moz-tools/bin/cbindgen" ]]; then
    CARGO_TARGET_DIR="$build/cbindgen-build" cargo install cbindgen --version 0.29.2 --locked --root "$build/moz-tools"
fi
if ! patch -f -Rs --dry-run -p1 -d "$src" < "$port/mozjs-port.patch" >/dev/null 2>&1; then
    patch -f -s -p1 -d "$src" < "$port/mozjs-port.patch"
fi
install -m 755 "$port/moz-cc" "$build/moz-cc"
ln -sfn moz-cc "$build/moz-cxx"

export MOZBUILD_STATE_PATH="$build/moz-state"
export CC="$build/moz-cc" CXX="$build/moz-cxx" HOST_CC=/usr/bin/clang HOST_CXX=/usr/bin/clang++
export AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib LLVM_CONFIG="$llvm/llvm-config"
export PKG_CONFIG_LIBDIR="$build/sysroot/lib/pkgconfig:$cache/gtk3-port/sysroot/lib/pkgconfig"

cd "$stage"
"$src/js/src/configure" --target=aarch64-unknown-linux-musl --disable-jit --disable-ctypes \
    --disable-tests --disable-debug --disable-debug-symbols --disable-jemalloc --disable-shared-js \
    --disable-bootstrap
if [[ "$build/runtime/rt/liblx.a" -nt dist/bin/js ]]; then
    rm -f dist/bin/js
fi
make -j "${JOBS:-8}"
bash "$port/install-mozjs-sdk.sh"
