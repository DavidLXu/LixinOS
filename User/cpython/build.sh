#!/usr/bin/env bash
# Invoked with the project's ARM bare-metal compiler on PATH.
set -euo pipefail
trap 'echo "CPython build failed in $PWD at line $LINENO" >&2; for log in build.out configure.out; do [[ ! -f $log ]] || tail -8 "$log" >&2; done' ERR
port=$(cd "$(dirname "$0")" && pwd)
work=$1
mkdir -p "$work"
cd "$work"
work=$PWD
src="$work/Python-3.14.5"
musl="$work/musl-1.2.5"
sysroot="$work/sysroot"
cache=$(dirname "$work")
build_key=$(cat "$cache/links-port/anchors.h" "$port/../links/tls.c" "$port"/*.c "$port"/*.h "$port"/*.S "$port"/*.py "$port"/*.ld "$port"/build.sh "$port"/../{console.h,tcc/api.c,tcc/lixin.h,file-time.h,ls/link.ld} | shasum -a 256)
if [[ -s cpython.pkg && -f complete && $(cat complete) == "$build_key" && -s package/bin/python3 ]]; then exit 0; fi

unpack() {
    local archive=$1 digest=$2 folder=$3 url=$4
    if [[ -f "$folder/.verified-$digest" ]]; then return; fi
    if ! echo "$digest  $archive" | shasum -a 256 -c - >/dev/null 2>&1; then
        curl --connect-timeout 15 -fLsS --retry 2 "$url" -o "$archive.tmp"
        echo "$digest  $archive.tmp" | shasum -a 256 -c -
        mv "$archive.tmp" "$archive"
    fi
    tar xf "$archive"
    touch "$folder/.verified-$digest"
}
unpack upstream.tar.xz 7e32597b99e5d9a39abed35de4693fa169df3e5850d4c334337ffd6a19a36db6 "$src" https://www.python.org/ftp/python/3.14.5/Python-3.14.5.tar.xz
unpack musl.tar.gz a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4 "$musl" https://distfiles.macports.org/musl/musl-1.2.5.tar.gz

unpack openssl.tar.gz 603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a openssl-3.5.9 https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz
unpack zlib.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 zlib-1.3.1 https://zlib.net/fossils/zlib-1.3.1.tar.gz
mkdir -p host libc target/Modules
if [[ ! -x host/python.exe ]]; then
    (cd host && "$src/configure" --without-ensurepip >configure.out 2>&1 &&
        make -j8 python.exe >build.out 2>&1)
fi
python3 "$port/musl-port.py" "$musl"
(cd libc
    if [[ ! -f config.mak ]]; then
        "$musl/configure" --target=aarch64-linux-musl --prefix="$sysroot" --disable-shared \
            CC=aarch64-none-elf-gcc AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib >configure.out 2>&1
    fi
    make -j8 >build.out 2>&1 && make install >install.out 2>&1
)
sh "$musl/tools/musl-gcc.specs.sh" "$sysroot/include" "$sysroot/lib" /dev/null |
    sed 's/crtbeginS.o%s//;s/crtendS.o%s//' >tool.specs
cat > cc <<'SH'
#!/bin/sh
exec aarch64-none-elf-gcc -specs="$(dirname "$0")/tool.specs" -D__linux__ -D__unix__ -mno-outline-atomics "$@"
SH
chmod +x cc
if [[ ! -f "$sysroot/lib/libz.a" ]]; then
    (cd zlib-1.3.1
        CC="$work/cc" AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib \
            ./configure --uname=Linux --static --prefix="$sysroot" >configure.out
        make -j8 libz.a >build.out
        cp zlib.h zconf.h "$sysroot/include/"
        cp libz.a "$sysroot/lib/"
    )
fi
ssl="$work/openssl-3.5.9"
# OpenSSL normally discovers getentropy dynamically on Unix. In a static
# LixinOS executable use musl's getrandom, backed by the virtio RNG.
rand_source="$ssl/providers/implementations/rands/seeding/rand_unix.c"
python3 - "$rand_source" <<'PYRAND'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
before = "static ssize_t syscall_random("
start = s.index("{", s.index(before))
end = s.index("\n}\n#endif /* defined(OPENSSL_RAND_SEED_GETRANDOM) */", start)
patched = (s[:start+1] + "\n    extern ssize_t getrandom(void *, size_t, unsigned);\n"
             "    return getrandom(buf, buflen, 0); /* LixinOS virtio RNG */" + s[end:])
if patched != s:
    p.write_text(patched)
PYRAND
if [[ ! -s "$ssl/libssl.a" || ! -s "$ssl/libcrypto.a" || "$rand_source" -nt "$ssl/libcrypto.a" ]]; then
    (cd "$ssl"
        CC="$work/cc" AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib \
            perl Configure linux-aarch64 no-shared no-dso no-module no-asm no-tests no-engine no-async \
            no-secure-memory no-afalgeng --with-rand-seed=getrandom --libdir=lib --prefix=/lib/openssl --openssldir=/lib/openssl -Os >configure.out
        sed -i.bak 's/-pthread/-D_REENTRANT/g' Makefile
        make -j10 build_generated >build.out 2>&1
        make -j10 libcrypto.a libssl.a >>build.out 2>&1
    )
fi
# configure only links probes. The final executable links posix.c instead.
echo 'long lx_posix(long n,long a,long b,long c,long d,long e,long f){return -38;} long lx_clone(){return -38;} void lx_thread_exit(){for(;;);} int lx_lookup_ipv4(){return -1;}' |
    ./cc -x c -c -o probe.o -
export SOURCE_DATE_EPOCH=1778371200
cd target
if [[ ! -f Makefile ]]; then
    CC="$work/cc" AR=aarch64-none-elf-ar RANLIB=aarch64-none-elf-ranlib READELF=aarch64-none-elf-readelf \
        CFLAGS="-Os -ffunction-sections -fdata-sections" CPPFLAGS="-D_GNU_SOURCE" LIBS="$work/probe.o" \
        ac_cv_file__dev_ptmx=no ac_cv_file__dev_ptc=no ac_cv_buggy_getaddrinfo=no \
        "$src/configure" --host=aarch64-linux-musl --build="$("$src/config.guess")" --prefix=/ \
        --with-build-python="$work/host/python.exe" --without-ensurepip --disable-test-modules >configure.out 2>&1
fi
awk 'BEGIN {print "*static*"} /^(array|_bisect|_json|_random|_struct|math|cmath|unicodedata|_heapq|_csv|_socket|select|fcntl|_posixsubprocess|_sha1|_sha2|_sha3|_md5|_blake2|_hmac|_pickle|binascii|zlib|_opcode|_queue|mmap|_statistics|_datetime|_contextvars|pyexpat) /' \
    Modules/Setup.stdlib >Modules/Setup.local.new
# Apple's make compares timestamps at one-second granularity. Generate the
# built-in table explicitly even when configure just wrote the default table.
if ! cmp -s Modules/Setup.local{,.new} || ! grep -q PyInit_unicodedata Modules/config.c; then
    mv Modules/Setup.local{.new,}
    "$src/Modules/makesetup" -c "$src/Modules/config.c.in" -s Modules \
        Modules/Setup.local Modules/Setup.stdlib Modules/Setup.bootstrap "$src/Modules/Setup"
    mv config.c Modules/config.c
    rm -f Modules/config.o
else
    rm Modules/Setup.local.new
fi
python3 - "$src/Python/ceval_macros.h" <<'PYHOOK'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
needle = "#define QSBR_QUIESCENT_STATE(tstate)\n"
if needle in s:
    p.write_text(s.replace(needle, "#if defined(__linux__) && defined(__aarch64__)\nextern void lx_vm_tick(void);\n#define QSBR_QUIESCENT_STATE(tstate) lx_vm_tick()\n#else\n#define QSBR_QUIESCENT_STATE(tstate) ((void)0)\n#endif\n"))
PYHOOK
make -j8 libpython3.14.a >build.out 2>&1
flags=(-Os -ffunction-sections -fdata-sections -I. -I"$src/Include")
"$work/cc" "${flags[@]}" -DPLATFORM='"lixinos"' -c "$src/Python/getplatform.c" -o lxplatform.o
# Compile the statically registered _ssl with CPython's built-in module flags.
"$work/cc" "${flags[@]}" -D_GNU_SOURCE -DPy_BUILD_CORE_BUILTIN \
    -I"$src/Include/internal" -I"$ssl/include" -c "$src/Modules/_ssl.c" -o lxssl.o
"$work/cc" "${flags[@]}" -nostdlib -static -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096,-T,"$port/program.ld" \
    "$sysroot/lib/crt1.o" "$port"/{main,memory,posix,interrupt,threads,spawn}.c "$port/switch.S" "$port/../tcc/api.c" lxplatform.o \
    -Dtls_free=lx_bearssl_free -I"$cache/bearssl-0.6/inc" -I"$cache/links-port" "$port/transport.c" "$port/../links/tls.c" \
    lxssl.o \
    "$cache/bearssl-0.6/arm/libbearssl.a" libpython3.14.a "$ssl/libssl.a" "$ssl/libcrypto.a" Modules/_hacl/*.o Modules/expat/*.o -lz -lc -lgcc -o "$work/cpython.elf"
python3 "$port/package.py" "$work"
echo "$build_key" > "$work/complete"
