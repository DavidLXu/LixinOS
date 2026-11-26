#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
ssl="$root/Kernel/target/user-build/cpython-port/openssl-3.5.9"
if [[ ${LIXINOS_CODEX_DYNAMIC:-1} == 1 ]]; then
    py="$root/Kernel/target/user-build/cpython-port"
    dyn="$root/Kernel/target/user-build/dynlink-port/package/lib"
    libs=$(rustc --print target-libdir --target aarch64-unknown-linux-musl)
    port="$root/User/rust"
    exec python3 - "$py" "$dyn" "$ssl" "$port" "$root/User/codex-exec/output.c" "$libs/self-contained/libunwind.a" "$@" <<'PY'
import os, shlex, subprocess, sys, tempfile

py, dyn, ssl, port, output_c, unwind_a = sys.argv[1:7]
raw_args = sys.argv[7:]

drop = {
    "-static", "-static-pie", "-no-pie", "-lc", "-lm", "-lpthread",
    "-ldl", "-lrt", "-lutil", "-lgcc_s", "-Wl,-Bstatic", "-Bstatic",
    "-Wl,-Bdynamic", "-Bdynamic",
}

def filter_args(items):
    out = []
    for item in items:
        if item.startswith("@") and len(item) > 1 and os.path.isfile(item[1:]):
            with open(item[1:], "r", encoding="utf-8") as fh:
                nested = shlex.split(fh.read())
            out.extend(filter_args(nested))
        elif item in drop:
            continue
        elif item == "-lunwind":
            out.append(unwind_a)
        else:
            out.append(item)
    return out

filtered = filter_args(raw_args)
with tempfile.NamedTemporaryFile("w", suffix=".rsp", delete=False, encoding="utf-8") as rsp:
    for arg in filtered:
        rsp.write(shlex.quote(arg) + "\n")
    rsp_path = rsp.name

try:
    cmd = [
        f"{py}/cc", "-fPIC", "-Os", "-ffunction-sections",
        "-DLX_DYNAMIC_RUNTIME", "-DLX_THREADED_API",
        f"-I{py}/musl-1.2.5/src/process", f"-I{ssl}/include",
        "-pie", "-nostdlib",
        "-Wl,-z,text,--gc-sections,--build-id=none,-z,max-page-size=4096,--dynamic-linker,/lib/ld-musl-aarch64.so.1",
        f"{py}/sysroot/lib/Scrt1.o", f"{py}/sysroot/lib/crti.o",
        f"{port}/spawn.c", f"{port}/metadata.c", output_c,
        f"@{rsp_path}",
        "-Wl,--start-group", f"{ssl}/libssl.a", f"{ssl}/libcrypto.a",
        "-lgcc", unwind_a, "-Wl,--end-group",
        "-Wl,-Bdynamic", f"{dyn}/libc.so",
        f"{py}/sysroot/lib/crtn.o",
    ]
    raise SystemExit(subprocess.call(cmd))
finally:
    try:
        os.unlink(rsp_path)
    except OSError:
        pass
PY
fi
exec "$root/User/rust/link.sh" "$@" -I"$ssl/include" "$root/User/codex-exec/output.c" "$ssl/libssl.a" "$ssl/libcrypto.a"
