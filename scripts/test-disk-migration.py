"""Verify migration of legacy snapshots and command history through QEMU."""
from pathlib import Path
import runpy
import struct
import tempfile

import disk_image as layout

serial = runpy.run_path(str(Path(__file__).with_name("test-user.py")))


def snapshot(generation, message):
    payload = struct.pack("<III", 2, 1, 0xffffffff) + b"/"
    payload += struct.pack("<II", 5, len(message)) + b"/keep" + message
    header = struct.pack("<8sQII", b"LIXINFS1", generation, len(payload), layout.checksum(payload))
    return (header + struct.pack("<I", layout.checksum(header))).ljust(512, b"\0") + payload


def test(directory, expanded):
    disk = directory / f"legacy-{expanded}.img"
    size = (136 if expanded else 8) << 20
    with disk.open("wb") as f:
        f.truncate(size)
        if expanded:
            f.write(snapshot(2, b"old slot\n"))
        base, slot = ((8 << 20, layout.SLOT) if expanded else (0, layout.OLD_SLOT))
        for generation in (4, 5):
            f.seek(base + generation % 2 * slot)
            f.write(snapshot(generation, f"kept {generation}\n".encode()))
        for n in range(1, 70):
            command = f"echo old-command-{n}".encode()
            record = struct.pack("<8sQH", b"LIXHIST1", n, len(command)) + command
            f.seek(layout.OLD_HISTORY + n % 65 * 1024)
            f.write(record + struct.pack("<I", layout.checksum(record)))
    layout.prepare(disk)
    assert disk.stat().st_size == layout.IMAGE_SIZE
    with disk.open("rb") as f:
        for gen in (4, 5):
            expected = snapshot(gen, f"kept {gen}\n".encode())
            f.seek(layout.SLOTS[gen % 2])
            assert f.read(len(expected)) == expected
    with serial["boot"](disk) as s:
        s.expect(b"\x1b[A\r", serial["SHELL"], b"\nold-command-69\n")
        s.expect(b"cat /keep\r", serial["SHELL"], b"\nkept 5\n")
        s.expect(b"echo new-history\r", serial["SHELL"])
        s.send(b"shutdown\r")
        assert s.vm.wait(timeout=10) == 0
    with serial["boot"](disk) as s:
        s.expect(b"\x1b[A\r", serial["SHELL"], b"\nnew-history\n")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as d:
        for expanded in (False, True):
            test(Path(d), expanded)
    print("Verified both old layouts, both snapshots, and history across reboot.")
