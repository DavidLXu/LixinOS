#!/usr/bin/env bash
# Export the current guest C runtime for the upstream Linux build machine.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
build="${LIXINOS_NODE_BUILD:-$root/Kernel/target/node-port}"
sdk="$build/sdk"
LIXINOS_GCC_BUILD="$build/runtime" bash "$root/User/gcc/build.sh" rt
musl="$root/Kernel/target/user-build/cpython-port/sysroot/lib"
mkdir -p "$sdk"
cp "$musl"/{crt1.o,crti.o,crtn.o,libc.a} "$sdk/"
cp "$build/runtime/rt/liblx.a" "$sdk/"
if ! command -v aarch64-none-elf-ar >/dev/null; then
    toolchain=("$root"/Kernel/target/user-build/arm-gnu-toolchain-*-aarch64-none-elf)
    export PATH="${toolchain[0]}/bin:$PATH"
fi
# Node links the complete C++ standard library.
aarch64-none-elf-ar d "$sdk/liblx.a" new.o
echo "$sdk"
