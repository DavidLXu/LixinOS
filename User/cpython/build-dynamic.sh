#!/usr/bin/env bash
# Relink the ported interpreter as PIE so musl can resolve extension symbols.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
py="$cache/cpython-port"
dyn="$cache/dynlink-port"
output="${1:-$dyn/python-package}"
mkdir -p "$output/bin" "$output/lib/cpython-native"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
if [[ ! -s $dyn/musl/lib/libc.so ]]; then bash "$port/../dynlink/build.sh"; fi
# Use the configured target's generated headers, module table and build rules.
# PIC objects are kept apart from the static release and reused across relinks.
build="$dyn/python-build"
mkdir -p "$build"
rsync -a --exclude='*.o' --exclude='*.a' --exclude='*.log' --exclude='*.out' \
    --exclude='*.d' --exclude='python' "$py/target/" "$build/"
cd "$build"
make -j"${JOBS:-10}" CFLAGS=-fPIC libpython3.14.a >pic-build.log 2>&1
ssl="$py/openssl-3.5.9"
bear="$cache/bearssl-0.6"
make -s -j"${JOBS:-10}" -C "$bear" BUILD="$dyn/bear-pic" CC=aarch64-none-elf-gcc \
    AR=aarch64-none-elf-ar CFLAGS="-Os -fPIC -ffunction-sections -fdata-sections" "$dyn/bear-pic/libbearssl.a"
mkdir -p "$dyn/zlib-pic"
(cd "$dyn/zlib-pic"
    if [[ ! -f Makefile ]]; then
        CC="$py/cc" AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib CFLAGS="-Os -fPIC" \
            "$py/zlib-1.3.1/configure" --uname=Linux --static >configure.log
    fi
    make -s -j"${JOBS:-10}" libz.a)
"$py/cc" -Os -fPIC -I. -I"$py/Python-3.14.5/Include" -D_GNU_SOURCE -DPy_BUILD_CORE_BUILTIN \
    -I"$py/Python-3.14.5/Include/internal" -I"$ssl/include" -c "$py/Python-3.14.5/Modules/_ssl.c" -o lxssl.o
"$py/cc" -Os -fPIE -DLX_DYNAMIC_RUNTIME -DLX_THREADED_API -mno-outline-atomics -nostdlib \
    -I. -I"$py/Python-3.14.5/Include" -Dtls_free=lx_bearssl_free \
    -I"$cache/bearssl-0.6/inc" -I"$cache/links-port" -Wl,-z,text,-pie,--export-dynamic,--gc-sections \
    -Wl,-z,max-page-size=4096,--dynamic-linker,/lib/ld-musl-aarch64.so.1 \
    "$py/sysroot/lib/Scrt1.o" "$py/sysroot/lib/crti.o" "$port"/{main,interrupt,spawn,transport}.c "$port/../links/tls.c" \
    "$py/target/lxplatform.o" lxssl.o "$dyn/bear-pic/libbearssl.a" libpython3.14.a \
    "$ssl/libssl.a" "$ssl/libcrypto.a" Modules/_hacl/*.o Modules/expat/*.o \
    "$dyn/zlib-pic/libz.a" "$dyn/musl/lib/libc.so" -lgcc "$py/sysroot/lib/crtn.o" -o "$output/bin/python3"
aarch64-none-elf-strip "$output/bin/python3"
for name in pip pip3; do cp "$dyn/package/bin/ld-musl" "$output/bin/$name"; done
