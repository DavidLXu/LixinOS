#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cache="$root/Kernel/target/user-build"
musl="$cache/cpython-port"
bash "$root/User/mujoco/build.sh"
mj="$cache/mujoco-port"
gnome="$cache/gnome-port"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
work="$cache/physics-build"
out="$cache/physics-package/bin"
mkdir -p "$work" "$out" "$cache/physics-package/lib/physics"
(cd "$root/User/mujoco" && xxd -i triple.mjb) | sed 's/^unsigned/static const unsigned/' > "$work/triple-model.h"
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$work/crt1.o"
"$musl/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I"$root/User/gcc" -c "$root/User/cpython/loader.c" -o "$work/crt0.o"
"$musl/cc" -std=gnu11 -Wall -Wextra -Os -ffunction-sections -fdata-sections -I"$root/User/tcc" -I"$mj/mujoco-4.0.1/include" -I"$work" \
    -nostartfiles -static -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 -Wl,-T,"$root/User/gcc/application.ld" \
    "$(aarch64-none-elf-gcc -print-file-name=crtbegin.o)" "$work/crt0.o" "$work/crt1.o" "$root/User/physics/app.c" "$root/User/gui/han-font.c" \
    -Wl,--start-group "$mj/libmujoco-engine.a" "$gnome/runtime/rt/liblx.a" "$gnome/alpine-sdk/usr/lib/libstdc++.a" -lm -lc -lgcc -Wl,--end-group "$(aarch64-none-elf-gcc -print-file-name=crtend.o)" -o "$work/physics"
aarch64-none-elf-strip -o "$out/physics.win3" "$work/physics"
install -m 644 "$mj/mujoco-4.0.1/LICENSE" "$cache/physics-package/lib/physics/LICENSE-MuJoCo"
install -m 644 "$cache/mujoco-package/lib/mujoco/LICENSE-libccd" "$cache/physics-package/lib/physics/"
