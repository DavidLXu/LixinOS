#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cache="$root/Kernel/target/user-build"
work="$cache/wasm3-port"
version=6b8bcb1e07bf26ebef09a7211b0a37a446eafd52
src="$work/wasm3-$version"
mkdir -p "$work/package/bin" "$work/package/lib/wasm3"
if [[ ! -f $work/source.tar.gz ]]; then
    curl -fsSL --retry 3 "https://codeload.github.com/wasm3/wasm3/tar.gz/$version" -o "$work/download"
    mv "$work/download" "$work/source.tar.gz"
fi
echo "d3d7a1cabbdc534e83c24be5181c637b03968f60a97fe0aac6cb515ee803b229  $work/source.tar.gz" | shasum -a 256 -c -
[[ -f $src/source/wasm3.h ]] || tar -xf "$work/source.tar.gz" -C "$work"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
py="$cache/cpython-port"
dyn="$cache/dynlink-port"
if [[ ! -f $dyn/package/lib/libc.so ]]; then bash "$root/User/dynlink/build.sh"; fi
"$py/cc" -O2 -std=gnu11 -Dd_m3HasWASI -I"$src/source" -I"$src/source/extra" \
    -fPIE -nostdlib -Wl,-pie,-z,text,-z,max-page-size=4096,--dynamic-linker,/lib/ld-musl-aarch64.so.1 \
    "$py/sysroot/lib/Scrt1.o" "$py/sysroot/lib/crti.o" "$src/platforms/app/main.c" "$src"/source/*.c \
    "$dyn/package/lib/libc.so" -lgcc "$py/sysroot/lib/crtn.o" -o "$work/package/bin/wasm3"
aarch64-none-elf-strip "$work/package/bin/wasm3"
cp "$src/LICENSE" "$work/package/lib/wasm3/"
