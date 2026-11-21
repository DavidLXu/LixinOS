#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cache="$root/Kernel/target/user-build"
out="$cache/settings-package"
gcc=${LIXINOS_GCC_BUILD:-"$root/Kernel/target/gcc-port"}
bash "$root/User/gcc/build.sh" rt
if ! command -v aarch64-none-elf-objcopy >/dev/null; then
    cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
    export PATH="${cross[0]}/bin:$PATH"
fi
musl="$cache/cpython-port"
mkdir -p "$out/bin" "$out/lib/settings" "$cache/settings-build"
obj="$cache/settings-build"
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$obj/crt1.o"
"$musl/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION \
    -I"$root/User/gcc" -c "$root/User/cpython/loader.c" -o "$obj/crt0.o"
"$musl/cc" -std=gnu11 -Wall -Wextra -Os -ffunction-sections -fdata-sections \
    -I"$root/User/tcc" -nostartfiles -static -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
    -Wl,-T,"$root/User/gcc/application.ld" "$obj/crt0.o" "$obj/crt1.o" \
    "$root/User/desktop/settings.c" "$root/User/gui/han-font.c" \
    -Wl,--start-group "$gcc/rt/liblx.a" -lc -lgcc -Wl,--end-group -o "$obj/settings"
rm -f "$out/bin/settings.win2"
aarch64-none-elf-strip -o "$out/bin/settings.win3" "$obj/settings"
{ echo; awk -F '\t' '!/^#/ && NF >= 3 {print $3}' /usr/share/zoneinfo/zone.tab; } > "$out/lib/settings/zones"
