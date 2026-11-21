#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
export PATH="$HOME/.cargo/bin:$PATH"
cache="$root/Kernel/target/user-build"
musl="$cache/cpython-port"
if [[ ! -x "$musl/cc" ]]; then
    bash "$root/scripts/build-user.sh"
fi
if ! command -v aarch64-none-elf-gcc >/dev/null; then
    cross=("$cache"/arm-gnu-toolchain-*-aarch64-none-elf)
    export PATH="${cross[0]}/bin:$PATH"
fi
bash "$root/User/gcc/build.sh" rt
crt="$cache/slint-crt"
mkdir -p "$crt"
aarch64-none-elf-objcopy --redefine-sym _start=lx_musl_start "$musl/sysroot/lib/crt1.o" "$crt/crt1.o"
"$musl/cc" -Os -ffreestanding -fno-builtin -DLX_APPLICATION -I"$root/User/gcc" \
    -c "$root/User/cpython/loader.c" -o "$crt/crt0.o"
export LIXINOS_RUST_APP=1 LIXINOS_RUST_APP_CRT="$crt"
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_LINKER="$root/User/rust/link.sh"
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_RUSTFLAGS='-Cpanic=abort -Crelocation-model=static -Clink-self-contained=no -Clinker-flavor=gcc'
target=aarch64-unknown-linux-musl
cargo build --locked --manifest-path "$port/Cargo.toml" --target-dir "$cache/slint-build" --release --target "$target"
package="$cache/slint-package"
mkdir -p "$package/bin" "$package/lib/slint-demo"
for executable in slint-demo slint-editor; do
    cp "$cache/slint-build/$target/release/$executable" "$package/bin/$executable.tmp"
    mv "$package/bin/$executable.tmp" "$package/bin/$executable"
done
cp "$port/COPYING" "$package/lib/slint-demo/COPYING"
echo "Slint demo package: $package"
