#!/usr/bin/env bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$port/../.." && pwd)
w="$root/Kernel/target/rust-port"
py="$root/Kernel/target/user-build/cpython-port"
target=aarch64-unknown-linux-musl
mkdir -p "$w"
export PATH="$HOME/.cargo/bin:$PATH"
cross=("$root"/Kernel/target/user-build/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="${cross[0]}/bin:$PATH"
export RUSTUP_TOOLCHAIN=${RUSTUP_TOOLCHAIN:-stable}
if [[ $(rustc -V) != 'rustc 1.100.0 (0ef1bc386 2026-11-09)' ]]; then
    echo "Select rustup toolchain 1.100.0 (with target $target)." >&2; exit 1
fi
fetch() {
    local dest=$1 url=$2 hash=$3 tmp
    if [[ -f $dest ]] && echo "$hash  $dest" | shasum -a 256 -c -; then return; fi
    tmp=$(mktemp "$dest.XXXXXX")
    if curl -fLsS --retry 3 "$url" -o "$tmp" && echo "$hash  $tmp" | shasum -a 256 -c -; then
        mv "$tmp" "$dest"
    else
        rm -f "$tmp"
        return 1
    fi
}
export CARGO_TARGET_DIR="$w/build"
export CC_aarch64_unknown_linux_musl="$py/cc" AR_aarch64_unknown_linux_musl=aarch64-none-elf-ar
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_LINKER="${LIXINOS_RUST_LINKER:-$port/link.sh}"
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_RUSTFLAGS='--cfg rustix_use_libc -Cpanic=abort -Crelocation-model=static -Clink-self-contained=no -Clinker-flavor=gcc'
export CARGO_PROFILE_RELEASE_OPT_LEVEL=s CARGO_PROFILE_RELEASE_DEBUG=0
export AARCH64_UNKNOWN_LINUX_MUSL_OPENSSL_INCLUDE_DIR="$py/openssl-3.5.9/include"
export AARCH64_UNKNOWN_LINUX_MUSL_OPENSSL_LIB_DIR="$py/openssl-3.5.9" AARCH64_UNKNOWN_LINUX_MUSL_OPENSSL_STATIC=1
# Build the current guest syscall adapter before either final link.
bash "$root/User/gcc/build.sh" rt
# Cargo cannot see changes to the external linker script or its C objects.
refresh_link() {
    local main=$1 elf=$2 dep
    for dep in "$port"/{spawn.c,metadata.c,link.sh} "$root/Kernel/target/gcc-port/rt/liblx.a" "$py/sysroot/lib/libc.a" "$root/User/cpython/program.ld"; do
        if [[ $dep -nt $elf ]]; then touch "$main"; break; fi
    done
}
case ${1:-all} in
rustc|all)
    fetch "$w/upstream.tar.gz" https://codeload.github.com/rust-lang/rust/tar.gz/refs/tags/1.100.0 629bd2b2160e08616c405a56a1e2de339c3a231bd529a83493daaa3c2ed83827
    [[ -d $w/rust-1.100.0/compiler ]] || tar xzf "$w/upstream.tar.gz" -C "$w"
    python3 "$port/prepare.py" "$w/rust-1.100.0"
    python3 "$port/prepare-macros.py" "$w/rust-1.100.0"
    cp "$port/compiler.lock" "$w/rust-1.100.0/Cargo.lock"
    refresh_link "$w/rust-1.100.0/compiler/rustc/src/main.rs" "$CARGO_TARGET_DIR/$target/release/rustc-main"
    (cd "$w/rust-1.100.0"
    export CG_CLIF_FORCE_GNU_AS=1 RUSTC_BOOTSTRAP=1 CFG_RELEASE=1.100.0 CFG_RELEASE_CHANNEL=stable
    export CFG_VERSION='1.100.0 (0ef1bc386 2026-11-09)' CFG_VER_DATE=2026-11-09 CFG_VER_HASH=0ef1bc386059596dada8fb65b50425aab2ea637a
    export CFG_COMPILER_HOST_TRIPLE=$target RUSTC_INSTALL_BINDIR=bin CFG_DEFAULT_CODEGEN_BACKEND=cranelift CFG_DEFAULT_LINKER=/bin/gcc
    cargo build --locked --target "$target" --release -p rustc-main)
    [[ ${1:-all} == all ]] || exit 0
    ;;
cargo|package) ;;
*) echo 'usage: build.sh [rustc|cargo|all|package]' >&2; exit 2;;
esac
if [[ ${1:-all} != package ]]; then
    # Use real LixinOS pipes for curl wakeups and atomic directory locks for SQLite.
    export CFLAGS_aarch64_unknown_linux_musl="-DHAVE_PIPE -DHAVE_PIPE2"
    export LIBSQLITE3_FLAGS='-DSQLITE_DEFAULT_UNIX_VFS="unix-dotfile"'
    fetch "$w/cargo.tar.gz" https://codeload.github.com/rust-lang/cargo/tar.gz/3acc79c0c841844675bc53a605922dbfc441e750 f2893d0562f9445fb2510f84ead290919f80b978e38432d91615b70f5033bf41
    if [[ ! -d $w/cargo-src/src ]]; then mkdir -p "$w/cargo-src"; tar xzf "$w/cargo.tar.gz" --strip-components=1 -C "$w/cargo-src"; fi
    fetch "$w/tar.crate" https://static.crates.io/crates/tar/tar-0.4.46.crate 3f6221d9a6003c78398e3b239969f352578258df48c8eb051caadae0015bc840
    [[ -d $w/tar-0.4.46/src ]] || tar xzf "$w/tar.crate" -C "$w"
    python3 "$port/prepare-cargo.py" "$w/cargo-src"
    cd "$w/cargo-src"
    export CARGO_TARGET_DIR="$w/cargo-build"
    refresh_link "$w/cargo-src/src/bin/cargo/main.rs" "$CARGO_TARGET_DIR/$target/release/cargo"
    cargo build --release --target "$target" --bin cargo --locked --features vendored-libgit2,curl/static-curl
fi
if [[ ${1:-all} != cargo ]]; then
    staged=$(mktemp -d "$w/package.XXXXXX")
    python3 "$port/package.py" "$staged"
    ln -sfn "$(basename "$staged")" "$w/current-package"
    echo "Rust toolchain package: $w/current-package"
fi
