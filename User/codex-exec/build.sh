#!/bin/bash
set -euo pipefail
port=$(cd "$(dirname "$0")" && pwd)
project=$(cd "$port/../.." && pwd)
stage=${LIXINOS_CODEX_BUILD:-"$HOME/Library/Caches/LixinOS/codex-exec-0.171.0"}
python3 "$port/prepare.py" "$stage"
stage=$(cd "$stage" && pwd)
cd "$stage/source/codex-rs"
cross=("$project"/Kernel/target/user-build/arm-gnu-toolchain-*-aarch64-none-elf)
export PATH="$HOME/.cargo/bin:${cross[0]}/bin:$PATH" RUSTUP_TOOLCHAIN=${RUSTUP_TOOLCHAIN:-stable}
export CARGO_TARGET_DIR="$stage/target" CARGO_INCREMENTAL=0 CARGO_PROFILE_RELEASE_DEBUG=0 CARGO_PROFILE_RELEASE_OPT_LEVEL=s CARGO_PROFILE_RELEASE_LTO=false CARGO_PROFILE_RELEASE_CODEGEN_UNITS=16
py="$project/Kernel/target/user-build/cpython-port"
export CC_aarch64_unknown_linux_musl="$project/User/codex-exec/cc" AR_aarch64_unknown_linux_musl=aarch64-none-elf-ar
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_LINKER="$project/User/codex-exec/link.sh"
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_RUSTFLAGS='--cfg lixinos --cfg rustix_use_libc --cfg mio_unsupported_force_poll_poll --cfg mio_unsupported_force_waker_pipe -Cpanic=abort -Crelocation-model=static -Clink-self-contained=no -Clinker-flavor=gcc'
export AARCH64_UNKNOWN_LINUX_MUSL_OPENSSL_INCLUDE_DIR="$py/openssl-3.5.9/include" AARCH64_UNKNOWN_LINUX_MUSL_OPENSSL_LIB_DIR="$py/openssl-3.5.9" AARCH64_UNKNOWN_LINUX_MUSL_OPENSSL_STATIC=1
# EL0 cannot read the ARM virtual counter. Use the guest OS entropy source supported by AWS-LC.
export AWS_LC_SYS_NO_JITTER_ENTROPY=1
export LIBSQLITE3_FLAGS='-DSQLITE_DEFAULT_UNIX_VFS="unix-dotfile"'
cargo build --locked --release --target aarch64-unknown-linux-musl -p codex-exec --bin lixinos-codex
