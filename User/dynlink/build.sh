#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
py="$cache/cpython-port"
work="$cache/dynlink-port"
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
mkdir -p "$work"/{obj,musl,package/bin,package/lib/dynlink}
if [[ ! -x "$py/cc" ]]; then (cd "$root" && bash scripts/build-user.sh); fi
src="$py/musl-1.2.5"
python3 "$root/User/cpython/musl-port.py" "$src"
cd "$work/musl"
if [[ ! -f config.mak ]]; then
    "$src/configure" --target=aarch64-linux-musl --prefix=/ --enable-shared \
        CC=aarch64-none-elf-gcc AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib \
        CFLAGS="-mno-outline-atomics" >configure.log
fi
for unit in "$root"/User/cpython/{posix,memory,threads}.c "$root"/User/cpython/switch.S \
            "$root"/User/tcc/api.c "$root"/User/gcc/terminal.c; do
    object="$work/obj/$(basename "${unit%.*}").o"
    "$py/cc" -fPIC -Os -fno-stack-protector -DLX_DYNAMIC_RUNTIME -DLX_THREADED_API \
        -mno-outline-atomics -c "$unit" -o "$object"
done
# The shared libc owns the syscall adapter and TLS for the entire process.
# Pass the objects as prerequisites and link inputs so re-running tracks them.
make -j"${JOBS:-10}" lib/libc.so "LIBCC=$(echo "$work"/obj/*.o) -lgcc" \
    -W "$src/ldso/dynlink.c" LDFLAGS="-Wl,-z,max-page-size=4096,-soname,libc.so"
aarch64-none-elf-strip -o "$work/package/lib/ld-musl-aarch64.so.1" lib/libc.so
cp "$work/package/lib/ld-musl-aarch64.so.1" "$work/package/lib/libc.so"
LIXINOS_GCC_BUILD="$work/sdk" bash "$root/User/gcc/build.sh" rt
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$py/sysroot/lib/crt1.o" "$work/crt1.o"
"$py/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I"$root/User/gcc" \
    -c "$root/User/cpython/loader.c" -o "$work/crt0.o"
"$py/cc" -Os -nostartfiles -static -s -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
    -Wl,-T,"$root/User/gcc/application.ld" "$work/crt0.o" "$work/crt1.o" "$port/start.c" \
    -Wl,--start-group "$work/sdk/rt/liblx.a" -lc -lgcc -Wl,--end-group -o "$work/package/bin/ld-musl"
