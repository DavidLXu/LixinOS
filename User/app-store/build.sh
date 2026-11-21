#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
gcc=${LIXINOS_GCC_BUILD:-"$root/Kernel/target/gcc-port"}
musl="$cache/cpython-port"
if [[ ! -x "$musl/cc" || ! -f "$gcc/rt/liblx.a" ]]; then bash "$root/User/gcc/build.sh" rt; fi
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
out="$cache/app-store-package"
obj="$cache/app-store-build"
mkdir -p "$out/bin" "$out/lib/app-store" "$obj"
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$obj/crt1.o"
"$musl/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I"$root/User/gcc" \
  -c "$root/User/cpython/loader.c" -o "$obj/crt0.o"
link_app() {
  local output=$1
  shift
  "$musl/cc" -std=gnu11 -Wall -Wextra -Os -ffunction-sections -fdata-sections \
    -I"$root/User/tcc" -nostartfiles -static -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
    -Wl,-T,"$root/User/gcc/application.ld" "$obj/crt0.o" "$obj/crt1.o" "$@" \
    -Wl,--start-group "$gcc/rt/liblx.a" -lc -lgcc -Wl,--end-group -o "$obj/linked"
  aarch64-none-elf-strip -o "$output" "$obj/linked"
}
link_app "$out/bin/app-store.win3" "$port/store.c" "$root/User/gui/han-font.c"
link_app "$out/lib/app-store/save" "$port/save.c"
cp "$port/store.py" "$port/ideas.py" "$out/lib/app-store/"
