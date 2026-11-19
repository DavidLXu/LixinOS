"""Exercise the EL0 editors, text utilities, and Git through the guest UART."""
from pathlib import Path
import runpy
import argparse
import struct
import tempfile

serial = runpy.run_path(str(Path(__file__).with_name("test-user.py")))
prompt = serial["SHELL"]


def legacy_image(disk):
    checksum = runpy.run_path(str(Path(__file__).with_name("export-git.py")))["checksum"]
    files = [(b"/", None), (b"/bin", None), (b"/bin/echo", b"LXcmd1\necho"),
             (b"/keep", b"local file"), (b"/limit.c", (b"x" * 255 + b"\n") * 4096)]
    data = bytearray(struct.pack("<I", len(files)))
    for name, value in files:
        data += struct.pack("<II", len(name), len(value) if value is not None else 0xffffffff)
        data += name + (value or b"")
    header = struct.pack("<8sQII", b"LIXINFS1", 1, len(data), checksum(data))
    with disk.open("wb") as stream:
        stream.truncate(8 << 20)
        stream.seek(4 << 20)
        stream.write((header + struct.pack("<I", checksum(header))).ljust(512, b"\0"))
        stream.write(data)


def test(guest_build=False):
    with tempfile.TemporaryDirectory() as directory:
        disk = Path(directory) / "disk.img"
        legacy_image(disk)
        with serial["boot"](disk) as s:
            if guest_build:
                for cmd in [
                    "mkdir /libexec",
                    "tcc /src/tinycc/bootstrap.c -o /tcc-new",
                    "lust emit-fn keycode /src/foreground/keycode.rs -o keys.c",
                    "/tcc-new -DAPP_KILO -I/src/tinycc/sdk /src/foreground/runtime.c /src/kilo/kilo.c keys.c -o /libexec/kilo",
                    "/tcc-new -DAPP_HTOP -I/src/tinycc/sdk /src/foreground/runtime.c /src/foreground/htop.c -o /libexec/htop",
                ]:
                    s.expect(cmd.encode()+b"\r",prompt)
                    s.expect(b"echo $?\r",prompt,b"\n0\n")
            s.expect(b"echo upgraded\r", prompt, b"\nupgraded\n")
            s.expect(b"cat keep\r", prompt, b"local file")
            s.expect(b"kilo limit.c\r", b"Ctrl-F = find")
            s.expect(b"X\x13", b"File size limit")
            s.expect(b"\x7f\x13", b"1048576 bytes written on disk")
            s.expect(b"\x11", prompt)
            s.expect(b"mkdir work\r", prompt)
            cwd = b"lixin:/work$ "
            s.expect(b"cd work\r", cwd)
            s.expect(b"git init\r", cwd, b"repository ready")
            s.expect(b"echo 'int value = 42; // note' > sample.c\r", cwd)
            s.expect(b"git add sample.c\r", cwd)
            s.expect(b"git commit -m first\r", cwd, b"first")
            s.expect(b"kilo sample.c\r", b"Ctrl-F = find", b"\x1b[31m42")
            s.expect(b"\x06value", b"\x1b[34mvalue")
            s.expect(b"\rnew_\x13", b"bytes written on disk")
            s.expect(b"\x11", cwd)
            s.expect(b"git diff\r", cwd, b"+int new_value = 42; // note")
            s.expect(b"kilo exact\r", b"Ctrl-F = find")
            s.expect(b"abc\x13", b"3 bytes written on disk")
            s.expect(b"\x11", cwd)
            s.expect(b"cat exact | wc -l\r", cwd, b"\n0\n")
            s.expect(b"htop\r", b"htop (console polling)")
            s.expect(b"q", cwd)
            s.expect(b"echo " + b"x"*255 + b" > big\r", cwd)
            for _ in range(8):
                s.expect(b"cat big >> big\r", cwd)
            s.expect(b"kilo big\r", b"Ctrl-F = find")
            s.expect(b"X\x13", b"65537 bytes written on disk")
            s.expect(b"\x11", cwd)
        with serial["boot"](disk) as s:
            s.expect(b"ls -l work/big\r", prompt, b"65537")
            s.expect(b"cat work/sample.c\r", prompt, b"int new_value = 42; // note")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--guest-build", action="store_true")
    test(parser.parse_args().guest_build)
    print("Passed EL0 editing, highlighting, search, Git diff and persistent large-file save.")
