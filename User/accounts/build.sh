#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
cache="$root/Kernel/target/user-build"
work="$cache/pam-port"
pkg="$work/package"
mkdir -p "$pkg/lib/pam" "$pkg/bin" "$work/include"
for item in 'linux-pam 32b4f544194b37fc70687e7b2d800be5b0a67a90a00de08112c244ffebbe60cc' 'linux-pam-dev 3728aa5142687eac0ade73fe0b0cfbdc4a45998b92fbdc3e8879b3e8cf37f671'; do
    read -r name digest <<< "$item"
    file="$work/$name-1.7.1-r2.apk"
    if [[ ! -f "$file" ]]; then
        curl -fsSL --retry 3 "https://dl-cdn.alpinelinux.org/alpine/v3.22/main/aarch64/${file##*/}" -o "$file.download"
        mv "$file.download" "$file"
    fi
    echo "$digest  $file" | shasum -a 256 -c -
done
tar -xOf "$work/linux-pam-1.7.1-r2.apk" usr/lib/libpam.so.0.85.1 > "$pkg/lib/pam/libpam.so.0"
tar -xf "$work/linux-pam-dev-1.7.1-r2.apk" -C "$work/include" --strip-components=2 usr/include/security
cat > "$pkg/lib/pam/lixinos-login" <<'PROFILE'
auth required /lib/pam/pam_unix.so nodelay
account required /lib/pam/pam_unix.so
PROFILE
cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
py="$cache/cpython-port"
dyn="$cache/dynlink-port"
[[ -f "$dyn/package/lib/libc.so" ]] || bash "$root/User/dynlink/build.sh"
python3 "$port/build-pam-module.py"
cp "$work/linux-pam-1.7.1/COPYING" "$pkg/lib/pam/COPYING"
for name in login passwd; do
    extra=("$port/$name.c")
    if [[ "$name" == login ]]; then extra+=("$root/User/rust/spawn.c" "$pkg/lib/pam/libpam.so.0"); fi
    "$py/cc" -Wall -O2 -I"$work/include" -I"$py/musl-1.2.5/src/process" -fPIE -nostdlib \
        -Wl,-pie,-z,text,-z,max-page-size=4096,-rpath,/lib/pam,--dynamic-linker,/lib/ld-musl-aarch64.so.1 \
        "$py/sysroot/lib/Scrt1.o" "$py/sysroot/lib/crti.o" "${extra[@]}" \
        "$dyn/package/lib/libc.so" -lgcc "$py/sysroot/lib/crtn.o" -o "$pkg/bin/$name"
    aarch64-none-elf-strip "$pkg/bin/$name"
done
