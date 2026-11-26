"""Exercise the musl VM ABI and real EL0 protection faults on private boot disks."""
import argparse
from pathlib import Path
import runpy
import subprocess
import tempfile

from disk_image import edit_files, prepare

ROOT = Path(__file__).resolve().parents[1]
threads = runpy.run_path(str(ROOT / "scripts/test-kernel-threads.py"))
native = threads["native"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rust", type=Path, required=True)
    parser.add_argument("--tcc", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        disk = work / "guest.img"
        prepare(disk)
        from os import environ
        env = dict(environ, LIXINOS_GCC_BUILD=str(work / "sdk"))
        subprocess.run(["bash", ROOT / "User/gcc/build.sh", "rt"], env=env, check=True)
        package = runpy.run_path(str(ROOT / "User/gcc/package.py"))
        binary = work / "vm-probe.elf"
        subprocess.run([work / "sdk/cc", "-O2", ROOT / "User/cpython/vm-probe.c", "-o", binary], check=True)
        files = work / "package"
        package["tool"](files, "/bin/vm-probe", binary, library="/lib/vm-probe", build_dir=work)
        with threads["boot"](disk, threads["args"](args.rust, disk, 2, "640M")) as guest:
            if not args.tcc:
                native["command"](guest, "tcc /src/tinycc/bootstrap.c -o /bin/tcc")
                native["command"](guest, "tcc --kernel @/src/kernel/sources -o /boot/kernel.elf")
            native["command"](guest, "echo ok > /vm-test")
            native["shutdown"](guest)
        tcc = args.tcc or work / "kernel.elf"
        if not args.tcc:
            tcc.write_bytes(native["read_file"](disk, "/boot/kernel.elf"))

        def install(rows):
            rows[b"/vm-input"] = (bytes(8), bytes(4096) + b"private")
            for path in files.rglob("*"):
                rows[("/" + str(path.relative_to(files))).encode()] = (
                    bytes(8), path.read_bytes() if path.is_file() else None)
        edit_files(disk, install)
        for kernel, ram in ((args.rust, "640M"), (tcc, "1536M")):
            with threads["boot"](disk, threads["args"](kernel, disk, 2, ram)) as guest:
                native["command"](guest, "/bin/vm-probe", expected=b"VM mappings OK")
                for mode in ("readonly", "unmapped", "past-eof"):
                    guest.send(f"/bin/vm-probe {mode}\r".encode())
                    output = guest.until(native["shell"])
                    assert b"unexpected" not in output and b"VM failure" not in output, output
                    guest.send(b"echo $?\r")
                    status = guest.until(native["shell"]).strip().splitlines()[-1]
                    assert status == (b"139" if kernel == args.rust else b"255"), (mode, status, output)
                    print(kernel.name, mode, "fault status", status.decode(), flush=True)
                native["command"](guest, "/bin/vm-probe", expected=b"VM mappings OK")
                native["shutdown"](guest)


if __name__ == "__main__":
    main()
