#!/usr/bin/env bash
# Link against the LixinOS syscall adapter, including when rustc uses -nodefaultlibs.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
py=$root/Kernel/target/user-build/cpython-port
gcc=${LIXINOS_GCC_BUILD:-$root/Kernel/target/gcc-port}
libs=$(rustc --print target-libdir --target aarch64-unknown-linux-musl)
port="$root/User/rust"
args=()
for arg do
    case "$arg" in
        -pie|-static-pie) ;;
        -lunwind) args+=("$libs/self-contained/libunwind.a");;
        *) args+=("$arg");;
    esac
done
# Only libunwind comes from the Rust distribution. Its self-contained libc
# targets Linux and must never take precedence over the guest's musl.
args+=(-nostartfiles -static -Wl,--gc-sections,--build-id=none,-z,max-page-size=4096)
if [[ ${LIXINOS_RUST_APP:-0} == 1 ]]; then
    lib=${LIXINOS_RUST_APP_CRT:-$gcc/current-package/lib/gcc}
    args=("$lib/crt0.o" "$lib/crt1.o" "${args[@]}" "-Wl,-T,$root/User/gcc/application.ld")
else
    args+=("-Wl,-T,$root/User/cpython/program.ld" "$py/sysroot/lib/crt1.o")
fi
exec "$py/cc" -Os -ffunction-sections -I"$py/musl-1.2.5/src/process" "$port/spawn.c" "$port/metadata.c" "${args[@]}" -Wl,--start-group "$gcc/rt/liblx.a" -lc -lgcc "$libs/self-contained/libunwind.a" -Wl,--end-group
