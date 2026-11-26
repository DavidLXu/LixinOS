#!/usr/bin/env bash
# CPython's libpython archive needs the same support objects used by /bin/python3.
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
c="$root/Kernel/target/user-build"
py="$c/cpython-port"
sdk="$c/freecad-port/sdk"
tmp=$(mktemp -d "$c/freecad-port/embed.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
cross=("$c"/arm-gnu-toolchain-*/bin)
export PATH="${cross[0]}:$PATH"
cd "$tmp"
inc=(-Os -ffunction-sections -fdata-sections -I"$py/target" -I"$py/Python-3.14.5/Include")
for unit in interrupt spawn transport; do
    "$c/gnome-port/cc" "${inc[@]}" -Dtls_free=lx_bearssl_free -I"$c/bearssl-0.6/inc" -I"$c/links-port" \
        -c "$root/User/cpython/$unit.c" -o "$unit.o"
done
"$c/gnome-port/cc" "${inc[@]}" -Dtls_free=lx_bearssl_free -I"$c/bearssl-0.6/inc" -I"$c/links-port" \
    -c "$root/User/links/tls.c" -o tls.o
{
    echo 'create libpython3.14.a'
    for lib in "$py/target/libpython3.14.a" "$py/openssl-3.5.9"/lib{ssl,crypto}.a "$c/bearssl-0.6/arm/libbearssl.a"; do
        echo "addlib $lib"
    done
    echo 'delete getplatform.o'
    for obj in ./*.o "$py/target"/lx{platform,ssl}.o "$py/target"/Modules/{_hacl,expat}/*.o; do
        echo "addmod $obj"
    done
    echo save
    echo end
} | aarch64-none-elf-ar -M
# CPython prefixes the public Expat symbols. Its private testing helper also
# needs a distinct name when Coin links its own Expat into the executable.
aarch64-none-elf-objcopy --redefine-sym _INTERNAL_trim_to_complete_utf8_characters=PyExpat_trim_utf8 \
    libpython3.14.a
# The guest interpreter entry point also verifies all built-in module symbols resolve.
"$c/gnome-port/cc" "${inc[@]}" "$root/User/cpython/main.c" libpython3.14.a -lz -lm -o embed-check
mkdir -p "$sdk/lib"
mv libpython3.14.a "$sdk/lib/libpython3.14.a"
