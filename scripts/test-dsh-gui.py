"""Run the native DSH window, real engine and local Messages fixture on a disposable volume."""

import argparse
from pathlib import Path
import runpy
import secrets
import struct
import tempfile

from disk_image import snapshot_headers

ROOT = Path(__file__).resolve().parents[1]
DSH = runpy.run_path(str(ROOT / "scripts/test-dsh.py"))
NATIVE = DSH["native"]
WRITE = runpy.run_path(str(ROOT / "User/rust/bootstrap-disk.py"))["write_volume"]


def fixture_files(seed):
    # Daily disks can contain several GiB. Copy only the installed runtime and
    # its shell utilities; the test creates its own fresh home and workspace.
    prefixes = (
        "/boot/",
        "/lib/gtk3/",
        "/lib/dsh/",
        "/lib/node/",
        "/lib/cxx-runtime/",
        "/lib/python3.14/",
        "/lib/cpython-3.14.5/",
    )
    programs = {
        "/bin/" + n
        for n in (
            "bash",
            "sh",
            "dsh",
            "node",
            "python3",
            "ld-musl",
            "shutdown",
            "q",
            "cd",
            "cp",
            "cat",
            "ls",
        )
    }
    rows = {}
    with seed.open("rb") as f:
        f.seek(snapshot_headers(f)[0][1] + 512)
        for _ in range(struct.unpack("<I", f.read(4))[0]):
            length, size = struct.unpack("<II", f.read(8))
            stamp, path = f.read(8), f.read(length).decode()
            if (
                path in programs
                or path.startswith(prefixes)
                or path in {"/lib/lxe-runtime.elf", "/lib/ld-musl-aarch64.so.1", "/lib/libc.so"}
            ):
                rows[path] = stamp, None if size == 0xFFFFFFFF else f.read(size)
            elif size != 0xFFFFFFFF:
                f.seek(size, 1)
    for path in list(rows):
        for parent in Path(path).parents:
            rows.setdefault(str(parent), (bytes(8), None))
    return rows


def run(args):
    rows = fixture_files(args.disk)
    package = ROOT / "Kernel/target/user-build/dsh-gui/package"
    for source in package.rglob("*"):
        rows["/" + source.relative_to(package).as_posix()] = (
            bytes(8),
            source if source.is_file() else None,
        )
    witness = "gui-" + secrets.token_hex(8)
    rows["/work"] = rows["/tmp"] = bytes(8), None
    rows["/work/witness.txt"] = bytes(8), witness.encode()
    rows["/test.py"] = bytes(8), ROOT / "User/dsh/gui/test-window.py"
    with tempfile.TemporaryDirectory(prefix="dsh-gui-") as folder:
        disk = Path(folder) / "test.img"
        WRITE(rows, b"", disk, with_loader=True)
        with DSH["messages_fixture"](witness) as (port, verified):
            cmd = [
                "env",
                f"LIXINOS_TCC_DISK={disk}",
                "LIXINOS_CODEX_SHELL=1",
                ROOT / "scripts/run-kernel.sh",
                "--kernel",
                args.kernel,
                "--release",
                args.release,
                "--serial",
            ]
            with NATIVE["guest"]["boot"](
                disk, cmd, prompt=b"LixinOS Bash (C shell)"
            ) as guest:
                guest.until(b"$ ")
                guest.expect(b"cd /work\r", b"lixin:/work$ ")
                for setting in (
                    "HOME=/work DSH_HOME=/work/.dsh PATH=/bin",
                    "DEEPSEEK_API_KEY=fixture-key DSH_TELEMETRY_DISABLED=1",
                    f"DEEPSEEK_BASE_URL=http://10.0.2.2:{port}/anthropic",
                ):
                    guest.expect(f"export {setting}\r".encode(), b"lixin:/work$ ")
                guest.send(b"python3 /test.py\r")
                output = guest.until(b"lixin:/work$ ", timeout=450)
                print(output.decode(errors="replace"), flush=True)
                NATIVE["shutdown"](guest)
            passed = b"DSH GUI passed" in output and verified.is_set()
        if args.screenshots:
            from PIL import Image
            from disk_image import disk_entries

            files, _ = disk_entries(disk)
            args.screenshots.mkdir(exist_ok=True, parents=True)
            for name in ("welcome", "trajectory", "first-event", "chat", "markdown", "timeout"):
                record = files.get("/" + name + ".rgb")
                if not record:
                    continue
                buf = record[1]
                w, h, compressed = struct.unpack_from("<3I", buf)
                pixels = buf[12:]
                if compressed:
                    pixels = b"".join(
                        struct.pack("<H", color) * n
                        for n, color in struct.iter_unpack("<HH", pixels)
                    )
                Image.frombytes("RGB", (w, h), bytes(pixels), "raw", "BGR;16").save(
                    args.screenshots / (name + ".png")
                )
        assert passed, "Expected a resumed GUI session and verified tool results"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, default=ROOT / "data/shared.img")
    parser.add_argument("--kernel", choices=("rust", "tcc"), default="rust")
    parser.add_argument("--release", default="latest")
    parser.add_argument("--screenshots", type=Path)
    run(parser.parse_args())
