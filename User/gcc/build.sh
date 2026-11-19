#!/usr/bin/env bash
# Canadian cross build. Build generators execute on the workstation, and the
# resulting cc1, GNU as and GNU ld execute against LixinOS's musl adapter.
set -eu -o pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
w=${LIXINOS_GCC_BUILD:-"$root/Kernel/target/gcc-port"}
py="$cache/cpython-port"
if [[ ! -x $py/cc ]]; then (cd "$root" && bash scripts/build-user.sh); fi
if ! command -v aarch64-none-elf-gcc >/dev/null; then
    bundle=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
    export PATH="${bundle[0]}/bin:$PATH"
fi
cross=$(cd "$(dirname "$(command -v aarch64-none-elf-gcc)")/.." && pwd)
mkdir -p "$w"
cd "$w"
w=$PWD
export LIXINOS_GCC_BUILD=$w
phase=${1:-all}
case $phase in
    all|fetch|rt|deps|binutils|gcc|package) ;;
    *) echo "Unknown GCC build phase: $phase" >&2; exit 2 ;;
esac
jobs=${JOBS:-10}
if [[ $phase == all || $phase == fetch ]]; then bash "$port/fetch.sh"; fi
prepare_runtime() {
    mkdir -p rt
    for src in "$port/../cpython"/{posix,memory,threads}.c "$port/../cpython/switch.S" "$port/../tcc/api.c" "$port/terminal.c"; do
        "$py/cc" -Os -ffunction-sections -fdata-sections -c "$src" -o "rt/$(basename "${src%.*}").o"
    done
    "$py/cc" -std=gnu++98 -fno-exceptions -fno-rtti -Os -ffunction-sections -c "$port/new.cc" -o rt/new.o
    aarch64-none-elf-ar rcsD rt/runtime.a rt/*.o
    if ! cmp -s rt/runtime.a rt/liblx.a; then mv rt/runtime.a rt/liblx.a; else rm rt/runtime.a; fi
    cat > "$w/cc" <<SH
#!/bin/sh
for arg do
    case "\$arg" in -c|-E|-S|-M|-MM|-print*|--version|-dump*) exec "$py/cc" -D__lixinos__ "\$@";; esac
done
exec "$py/cc" -D__lixinos__ "\$@" -nostartfiles "$py/sysroot/lib/crt1.o" -static -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096,-T,"$port/../cpython/program.ld",--start-group "$w/rt/liblx.a" -lc -lgcc -Wl,--end-group
SH
    chmod +x "$w/cc"
    cxxheaders=$(ls -d "$cross"/aarch64-none-elf/include/c++/*)
    cat > "$w/cxx" <<SH
#!/usr/bin/env bash
args=()
for arg do
    case \$arg in *.c) args+=(-x c++ "\$arg" -x none);; *) args+=("\$arg");; esac
done
exec "$w/cc" -isystem "$cxxheaders" -isystem "$cxxheaders/aarch64-none-elf" -std=gnu++98 -fno-rtti -fno-exceptions -fno-threadsafe-statics "\${args[@]}"
SH
    chmod +x "$w/cxx"
}
# Upstream makefiles do not track the libraries added by our compiler driver.
relink_if_needed() {
    local binary=$1 dep
    if [[ -f $binary ]]; then
        for dep in "$w/rt/liblx.a" "$py/sysroot/lib/libc.a" "$port/../cpython/program.ld"; do
            if [[ $dep -nt $binary ]]; then rm "$binary"; break; fi
        done
    fi
}
ccflags=(-Os -ffunction-sections -fdata-sections)
common=(--host=aarch64-unknown-linux-gnu --disable-nls --disable-shared)
tools=(CC="$w/cc" CXX="$w/cxx" AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib NM=aarch64-none-elf-nm "CFLAGS=${ccflags[*]}" "CXXFLAGS=${ccflags[*]}")
prefix="$w/deps"
if [[ $phase == rt || $phase == all ]]; then prepare_runtime; fi
if [[ $phase == deps || $phase == all ]]; then
    for dep in gmp-6.2.1 mpfr-4.1.0 mpc-1.2.1; do
        mkdir -p "build-$dep"
        opts=()
        case $dep in gmp-*) opts=(--disable-assembly);; mpfr-*) opts=(--with-gmp="$prefix");; mpc-*) opts=(--with-gmp="$prefix" --with-mpfr="$prefix");; esac
        (cd "build-$dep"; [[ -f Makefile ]] || "$w/$dep/configure" "${common[@]}" --prefix="$prefix" "${opts[@]}" "${tools[@]}" >configure.log 2>&1
        make -j"$jobs" >build.log 2>&1 && make install >>build.log 2>&1)
    done
fi
if [[ $phase == binutils || $phase == all ]]; then
    src="$w/binutils-2.43.1"
    if ! grep -q 'lixin_publish (bfd' "$src/bfd/opncls.c"; then
        (cd "$src" && patch -f -p1 < "$port/binutils.patch")
    fi
    if ! (cd "$src" && patch -f -R --dry-run -s -p1 < "$port/binutils-build-volume.patch") >/dev/null 2>&1; then
        (cd "$src" && patch -f -p1 < "$port/binutils-build-volume.patch")
    fi
    if ! grep -q 'lx_save_buffer' "$src/bfd/opncls.c"; then
        (cd "$src" && patch -f -p1 < "$port/binutils-wide.patch")
    fi
    if ! grep -q 'memory_capacity (bfd_size_type' "$src/bfd/bfdio.c"; then
        (cd "$src" && patch -f -p1 < "$port/binutils-buffer.patch")
    fi
    mkdir -p build-binutils
    (cd build-binutils
    [[ -f Makefile ]] || "$w/binutils-2.43.1/configure" "${common[@]}" --target=aarch64-none-elf --disable-werror --disable-gprofng --disable-gdb --disable-gprof --disable-sim --disable-gold --disable-plugins --without-zstd --with-system-zlib "${tools[@]}" >configure.log 2>&1
    relink_if_needed gas/as-new
    relink_if_needed ld/ld-new
    make -j"$jobs" MAKEINFO=true all-gas all-ld all-binutils >build.log 2>&1)
fi
if [[ $phase == gcc || $phase == all ]]; then
    python3 "$port/patch.py" "$w/gcc-4.8.5"
    mkdir -p build-gcc
    (cd build-gcc
    [[ -f Makefile ]] || "$w/gcc-4.8.5/configure" "${common[@]}" --target=aarch64-none-elf --prefix=/ --with-gnu-as --with-gnu-ld --enable-languages=c --disable-bootstrap --without-headers --disable-multilib --disable-werror --disable-threads --disable-libssp --disable-libgomp --disable-libatomic --disable-libquadmath --disable-libsanitizer --disable-lto --disable-plugin --with-gmp="$prefix" --with-mpfr="$prefix" --with-mpc="$prefix" "${tools[@]}" >configure.log 2>&1
    relink_if_needed gcc/xgcc
    relink_if_needed gcc/cc1
    make -j"$jobs" MAKEINFO=true CXX_FOR_BUILD='c++ -std=gnu++98' all-gcc >build.log 2>&1)
fi
if [[ $phase == package || $phase == all ]]; then
    staged=$(mktemp -d "$w/package.XXXXXX")
    python3 "$port/package.py" "$staged"
    ln -sfn "$(basename "$staged")" "$w/current-package"
    echo "GCC package: $w/current-package"
fi
