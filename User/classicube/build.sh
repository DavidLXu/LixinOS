#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
build="$cache/classicube-port"
upstream="$cache/classicube-upstream"
rev=74345260e5af87ba0b850a2048399864e38f33c8
mkdir -p "$build"
if [[ ! -d "$upstream/.git" ]]; then
    git init -q "$upstream"
fi
if ! git -C "$upstream" cat-file -e "$rev^{commit}" 2>/dev/null; then
    git -C "$upstream" fetch --depth=1 https://github.com/ClassiCube/ClassiCube.git "$rev"
fi
musl="$cache/cpython-port"
LIXINOS_GCC_BUILD="$build/runtime" bash "$root/User/gcc/build.sh" rt
if ! command -v aarch64-none-elf-gcc >/dev/null; then
    tools=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
    export PATH="${tools[0]}/bin:$PATH"
fi
src="$build/source"
mkdir -p "$src"
git -C "$upstream" archive "$rev" src misc/cc_textures.zip license.txt credits.txt | tar -x -C "$src"
python3 "$port/prepare.py" "$src/src"
flags=(-O2 -std=gnu11 -D_GNU_SOURCE= -ffunction-sections -fdata-sections -I"$src/src" -include "$port/config.h")
mkdir -p "$build/obj" "$build/package/bin" "$build/package/lib/classicube"
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$build/obj/crt1.o"
"$musl/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I"$root/User/gcc" -c "$root/User/cpython/loader.c" -o "$build/obj/crt0.o"
objects=()
for file in "$src/src/"*.c "$port/window.c"; do
    case "${file##*/}" in _*|LWeb*|LScreens*|LWidgets*|LSColors*|LBackend*|Resources.c) continue;; esac
    obj="$build/obj/$(basename "${file%.c}").o"; objects+=("$obj")
    "$musl/cc" "${flags[@]}" -c "$file" -o "$obj"
done
"$musl/cc" -nostartfiles -static -s -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
    -Wl,-T,"$root/User/gcc/application.ld" "$build/obj/crt0.o" "$build/obj/crt1.o" "${objects[@]}" \
    -Wl,--start-group "$build/runtime/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group -o "$build/package/bin/classicube"
cp "$src/misc/cc_textures.zip" "$build/package/lib/classicube/textures.zip"
cp "$src"/{license.txt,credits.txt} "$build/package/lib/classicube/"
