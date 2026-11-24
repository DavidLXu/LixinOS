#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
ssl="$root/Kernel/target/user-build/cpython-port/openssl-3.5.9"
exec "$root/User/rust/link.sh" "$@" -I"$ssl/include" "$root/User/codex-exec/output.c" "$ssl/libssl.a" "$ssl/libcrypto.a"
