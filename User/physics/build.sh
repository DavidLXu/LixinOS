#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cache="$root/Kernel/target/user-build"
musl="$cache/cpython-port"
gcc=${LIXINOS_GCC_BUILD:-"$root/Kernel/target/gcc-port"}
[[ -f "$gcc/rt/liblx.a" ]] || bash "$root/User/gcc/build.sh" rt
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
work="$cache/physics-build"
out="$cache/physics-package/bin"
mkdir -p "$work" "$out" "$cache/physics-package/lib/physics"
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$work/crt1.o"
"$musl/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I"$root/User/gcc" -c "$root/User/cpython/loader.c" -o "$work/crt0.o"
"$musl/cc" -std=gnu11 -Wall -Wextra -Os -ffunction-sections -fdata-sections -I"$root/User/tcc" \
    -nostartfiles -static -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 -Wl,-T,"$root/User/gcc/application.ld" \
    "$work/crt0.o" "$work/crt1.o" "$root/User/physics/app.c" "$root/User/gui/han-font.c" \
    -Wl,--start-group "$gcc/rt/liblx.a" -lm -lc -lgcc -Wl,--end-group -o "$work/physics"
aarch64-none-elf-strip -o "$out/physics.win3" "$work/physics"
