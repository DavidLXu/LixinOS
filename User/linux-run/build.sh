#!/usr/bin/env bash
# Build the LixinOS runner separately from the Debian-targeted test executable.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
w="$root/Kernel/target/linux-user"
py="$cache/cpython-port"
mkdir -p "$w"
stage=$(mktemp -d "$w/build.XXXXXX")
trap 'rm -rf "$stage"' EXIT

fetch() {
    local url=$1 name=$2 digest=$3
    if [[ ! -f "$w/$name" ]]; then
        curl -fLsS --retry 2 "$url" -o "$stage/$name"
        printf '%s  %s\n' "$digest" "$stage/$name" | shasum -a 256 -c -
        mv "$stage/$name" "$w/$name"
    fi
    printf '%s  %s\n' "$digest" "$w/$name" | shasum -a 256 -c -
}
rev=90fe88ca71794d29eecddc4b8925d68b79a2c342
fetch "https://codeload.github.com/sylirre/arm64emu-user/tar.gz/$rev" arm64emu.tar.gz \
    366021212b4f68b34e724092f1b1f7f14d7ee88ed1f9a74471779e693050d7e7
debian=https://deb.debian.org/debian/pool/main/c/cross-toolchain-base
version=2.36-8cross1
fetch "$debian/libc6-arm64-cross_${version}_all.deb" libc6-arm64-cross.deb \
    91936cbbee75771c360ed16513f6e734a25ca5517fe7c3e13dfd7835d7553186
fetch "$debian/libc6-dev-arm64-cross_${version}_all.deb" libc6-dev-arm64-cross.deb \
    5e4cf6abf0e89e89c4b3006bb8b7dc47aa831df25d94df9134929b8b968cacb4
fetch "$debian/linux-libc-dev-arm64-cross_6.1.4-1cross1_all.deb" linux-libc-dev-arm64-cross.deb \
    66457f015d16b7d372db6591cff38db0da928362ff2ea1fa75491e28f6904d81
mkdir "$stage/src" "$stage/sysroot" "$stage/obj"
tar xf "$w/arm64emu.tar.gz" -C "$stage/src" --strip-components 1
for file in libc6-arm64-cross.deb libc6-dev-arm64-cross.deb linux-libc-dev-arm64-cross.deb; do
    tar -xOf "$w/$file" data.tar.xz | tar -xJf - -C "$stage/sysroot"
done

LIXINOS_GCC_BUILD="$w/runtime" bash "$root/User/gcc/build.sh" rt
if ! command -v aarch64-none-elf-gcc >/dev/null; then
    cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
    export PATH="${cross[0]}/bin:$PATH"
fi
patch -s -d "$stage/src" -p1 < "$port/predecode.patch"
core="$stage/src/src/core"
for unit in "$core"/{cpu,decode,sysreg,exec_fpsimd}.c "$core/../predecode.c" "$port"/{entry,memory,syscalls,state,shm,userns,threads,highmem,events,messages}.c; do
    # Use the core's single-thread CASP fallback instead of requiring libatomic.
    flags=()
    if [[ $unit == "$core/decode.c" ]]; then flags=(-U__SIZEOF_INT128__); fi
    case "${unit##*/}" in
        predecode.c|decode.c|exec_fpsimd.c|memory.c|highmem.c) flags+=(-O2) ;;
    esac
    "$py/cc" -Os -std=gnu11 -D_GNU_SOURCE -ffunction-sections -fdata-sections -fno-strict-aliasing \
        -fno-math-errno ${flags[@]+"${flags[@]}"} -I"$core" -I"$core/.." -c "$unit" -o "$stage/obj/$(basename "${unit%.c}").o"
done
# Multiplex Linux pthreads with the existing LixinOS cooperative runtime. One
# application can have more logical threads than the kernel's scheduler slots.
"$py/cc" -Os -DLX_PTHREAD_COOPERATIVE_ONLY -ffunction-sections -fdata-sections \
    -c "$root/User/cpython/threads.c" -o "$stage/obj/thread-backend.o"
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$py/sysroot/lib/crt1.o" "$stage/crt1.o"
"$py/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I"$root/User/gcc" \
    -c "$root/User/cpython/loader.c" -o "$stage/crt0.o"
pkg="$w/package"
mkdir -p "$pkg/bin" "$pkg/lib/aarch64-linux-gnu" "$pkg/share/licenses/linux-run"
"$py/cc" -nostartfiles -static -s -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096 \
    -Wl,-T,"$root/User/gcc/application.ld" "$stage/crt0.o" "$stage/crt1.o" "$stage/obj/"*.o \
    -Wl,--start-group "$w/runtime/rt/liblx.a" -lc -lm -lgcc -Wl,--end-group -o "$pkg/bin/linux-run"

sdk="$stage/sysroot/usr/aarch64-linux-gnu"
lib="$sdk/lib"
# Only Linux startup objects, Linux headers and the distribution's libc enter this PIE.
for program in hello pthread-probe chromium-probe; do
    clang --target=aarch64-linux-gnu --sysroot="$sdk" -isystem "$sdk/include" -O2 -fPIE \
        -c "$port/$program.c" -o "$stage/$program.o"
    aarch64-none-elf-ld -m aarch64linux -pie --dynamic-linker /lib/ld-linux-aarch64.so.1 \
        -rpath-link "$lib" -o "$pkg/bin/linux-$program" "$lib/Scrt1.o" "$lib/crti.o" "$stage/$program.o" \
        "$lib/libc.so.6" "$lib/libc_nonshared.a" "$lib/crtn.o"
done
# A Linux NSS plugin gives glibc clients access to the LixinOS DNS resolver.
clang --target=aarch64-linux-gnu --sysroot="$sdk" -isystem "$sdk/include" -O2 -fPIC \
    -c "$port/nss-lixinos.c" -o "$stage/nss.o"
aarch64-none-elf-ld -m aarch64linux -shared -soname libnss_lixinos.so.2 \
    -o "$pkg/lib/aarch64-linux-gnu/libnss_lixinos.so.2" "$stage/nss.o" "$lib/libc.so.6"
mkdir -p "$pkg/lib/linux-run"
printf 'hosts: lixinos\n' > "$pkg/lib/linux-run/nsswitch.conf"
cp "$lib/ld-linux-aarch64.so.1" "$pkg/lib/"
cp "$lib/libc.so.6" "$pkg/lib/aarch64-linux-gnu/"
cp "$stage/src/LICENSE" "$stage/src/NOTICE" "$pkg/share/licenses/linux-run/"
cp "$stage/sysroot/usr/share/doc/libc6-arm64-cross/copyright" "$pkg/share/licenses/linux-run/Debian-glibc"
echo "$pkg"
