"""Run the system rebuild inside QEMU. Optionally retain the verified guest disk."""
import argparse
import os
from pathlib import Path
import runpy
import tempfile

guest = runpy.run_path(str(Path(__file__).with_name("test-user.py")))


def check(console, cwd, command, expected=b""):
    prompt = f"lixin:{cwd}$ ".encode()
    console.send(command.encode() + b"\r")
    output = console.until(prompt, timeout=240)
    assert expected in output, output
    console.send(b"echo $?\r")
    assert b"\n0\n" in console.until(prompt), output


def verify(disk):
    with guest["boot"](disk) as s:
        check(s, "/", "mkdir native")
        check(s, "/native", "cd native")
        check(s, "/native", "tcc /src/tinycc/bootstrap.c -o tcc-new")
        check(s, "/native", "tcc /src/build/rebuild.c -o rebuild")
        check(s, "/native", "./rebuild", b"python: built (exit 0)")
        check(s, "/native", "./ls -1 .", b"desktop")
        check(s, "/native", "./echo native | ./grep ative | ./wc -l", b"\n1\n")
        check(s, "/native", "./find . -maxdepth 1 -name rebuild -print0 | ./xargs -0 ./stat",
              b"Type: regular file")
        check(s, "/native", "./printf '%s=%d\\n' native 42", b"\nnative=42\n")
        check(s, "/native", "./test -s rebuild")
        check(s, "/native", "./pwd", b"\n/native\n")
        check(s, "/native", "./uname -m", b"aarch64")
        check(s, "/native", "./cpuinfo", b"scope=guest_scheduler")
        check(s, "/native", "./bash --version", b"LixinOS Bash")
        check(s, "/native", "./rust-hello", b"Hello from LixinOS Rust: 42")
        check(s, "/native", "echo 'print(sum(x*x for x in range(20)))' > smoke.py")
        check(s, "/native", "./python smoke.py", b"\n2470\n")
        check(s, "/native", """printf q | ./python -c 'import select, sys; p = select.poll(); p.register(sys.stdin, select.POLLIN); a = [1, 2, 3]; n = len(a[:-1]); print(f"{n}", bool(p.poll(0)), sys.stdin.read(1))'""",
              b"\n2 True q\n")
        s.send(b"q\r")
        assert s.vm.wait(timeout=10) == 0
    with guest["boot"](disk) as s:
        check(s, "/", "/native/find /native -maxdepth 1 -name grep", b"/native/grep")
        check(s, "/", "/native/cpuinfo", b"guest_busy_percent=")
        check(s, "/", "/native/python /native/smoke.py", b"2470")
        check(s, "/", "/native/rust-hello", b"Hello from LixinOS Rust")
        s.send(b"q\r")
        assert s.vm.wait(timeout=10) == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--save-disk", type=Path, help="new destination for the boot-tested disk")
    dest = parser.parse_args().save_disk
    if dest and dest.exists():
        parser.error(f"disk already exists: {dest}")
    with tempfile.TemporaryDirectory(dir=dest.resolve().parent if dest else None) as directory:
        disk = Path(directory).resolve() / "native.img"
        verify(disk)
        if dest:
            os.link(disk, dest)  # exclusive publication after guest shutdown
    print("Verified the guest system rebuild and persisted ARM64 programs across reboot.")
