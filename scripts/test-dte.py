"""Exercise the native dte package on disposable disks under both kernels."""
import argparse
from pathlib import Path
import runpy
import subprocess
import tempfile
import time

from disk_image import require_local

root = Path(__file__).resolve().parents[1]
native = runpy.run_path(str(root / "scripts/test-native-kernel.py"))
boot = native["guest"]["boot"]
prompt = native["shell"]
package = root / "Kernel/target/user-build/dte-port/package"


def check(kernel, release, source, disk):
    subprocess.run(["cp", "-c", source, disk], check=True)
    subprocess.run([
        "python3", root / "User/gcc/install.py", "--toolchain", "dte", package, disk,
    ], check=True)
    launch = [root / "scripts/run-kernel.sh", "--kernel", kernel, "--release", release, "--serial"]

    with boot(disk, launch, prompt=b"LixinOS Bash (C shell)") as serial:
        serial.expect(b"cd /\r", prompt)
        native["command"](serial, "dte -V", expected=b"1.11.1")
        native["command"](serial, "/bin/dte first.c > redirected", status=74,
                          expected=b"interactive LixinOS terminal")
        native["command"](serial, "echo 'int before;' > /dte-first.c")
        native["command"](serial, "echo 'int second;' > /dte-second.c")
        native["command"](serial, "echo 'int diagnostic;' >> /dte-second.c")
        serial.expect(b"/bin/dte /dte-first.c /dte-second.c\r", b"\x1b[?1049h")
        serial.until(b"\x1b[?25h")
        serial.expect(b"\x14", b" second;")
        serial.until(b"\x1b[?25h")
        serial.expect(b"\x02", b" before;")
        serial.until(b"\x1b[?25h")
        serial.expect(b"\x06", b"/\x1b")
        serial.expect(b"before", b"/before")
        serial.expect(b"\r", b" before;")
        serial.send(b"\x1b[H")
        serial.send(b"\x1b[200~// \xe4\xb8\xad\xe6\x96\x87\r\x1b[20")
        time.sleep(.05)  # The closing paste sequence can straddle console reads.
        serial.send(b"1~")
        serial.until("中文".encode())
        serial.send(b"\x1a")
        serial.send(b"\x19")
        serial.send(b"\x13")
        serial.expect(b"\x05", b":\x1b")
        diagnostic = b"compile -1s gcc /bin/echo '/dte-second.c:2:5: Test diagnostic'"
        serial.expect(diagnostic, b"Test diagnostic")
        serial.expect(b"\r", b"Test diagnostic")
        serial.until(b"\x1b[3;10H\x1b[?25h")
        serial.expect(b"\x11", prompt)
        native["command"](serial, "echo 'int good;' > /dte-broken.c")
        native["command"](serial, "echo 'int broken = ;' >> /dte-broken.c")
        serial.expect(b"dte /dte-broken.c\r", b"\x1b[?25h")
        serial.expect(b"\x05", b":\x1b")
        serial.expect(b"tcc /dte-broken.c -o /dte-broken", b"-o /dte-broken")
        serial.expect(b"\r", b"expression expected")
        serial.until(b"\x1b[3;6H\x1b[?25h")
        serial.expect(b"\x11", prompt)
        native["command"](serial, "cat /dte-first.c", expected=b"\n// " + "中文".encode())
        native["command"](serial, "cat /dte-second.c", expected=b"\nint second;\n")
        serial.expect(b"dte /dte-new.c\r", b"\x1b[?25h")
        serial.send(b"new file\x13")
        serial.expect(b"\x11", prompt)
        native["shutdown"](serial)

    assert native["read_file"](disk, "/dte-first.c") == "// 中文\nint before;\n".encode()
    assert native["read_file"](disk, "/dte-new.c") == b"new file\n"
    print(f"{kernel}: verified UTF-8 edits, search, undo/redo, saves and diagnostic jumps", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", choices=("rust", "tcc", "both"), default="both")
    parser.add_argument("--release", default="latest")
    parser.add_argument("--disk", type=Path, required=True, help="initialized, powered-off seed disk")
    args = parser.parse_args()
    source = args.disk.resolve(strict=True)
    require_local(source, source.stat())
    release = args.release
    if release == "latest":
        release = subprocess.check_output(
            ["python3", root / "scripts/kernel-release.py", "latest"], text=True,
        ).strip()
        if not release:
            parser.error("no paired kernel release is available")
    with tempfile.TemporaryDirectory(prefix="lixin-dte-") as temp:
        for kernel in ("rust", "tcc") if args.kernel == "both" else (args.kernel,):
            check(kernel, release, source, Path(temp) / f"{kernel}.img")


if __name__ == "__main__":
    main()
