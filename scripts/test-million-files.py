"""Create a million files in the guest, persist a 1 MiB file, and reboot."""
from pathlib import Path
import re
import runpy
import struct
import tempfile

from disk_image import SLOTS

serial = runpy.run_path(str(Path(__file__).with_name("test-user.py")))
boot = serial["boot"]


def command(s, line, expected=b""):
    s.send(line.encode() + b"\r")
    reply = s.until(b"$ ", timeout=180).split(b"\n", 1)[1]
    assert b"shell: " not in reply and expected in reply, (line, reply[-512:])
    return reply


def test(disk):
    with boot(disk) as s:
        command(s, """echo '#include <stdio.h>' > names.c""")
        command(s, """echo 'int main(void){for(int i=0;i<100000;i++)printf("%06d%c",i,10);return 0;}' >> names.c""")
        command(s, "tcc names.c -o names")
        command(s, "./names > manifest")
        assert re.search(rb"700000  [^\n]+ UTC  manifest\n", command(s, "ls -l manifest"))
        command(s, "echo x > big")
        for _ in range(19):
            command(s, "cat big >> big")
        for i in range(10):
            command(s, f"mkdir /batch{i}")
            command(s, f"cd /batch{i}")
            command(s, "touch --files-from /manifest")
        command(s, "echo last > 099999")
        command(s, "mkdir /excess")
        command(s, "cd /excess")
        s.send(b"touch --files-from /manifest\r")
        assert b"filesystem full" in s.until(b"$ ", timeout=180)
        s.expect(b"ls 000000\r", b"lixin:/excess$ ", b"path not found")

        s.send(b"shutdown\r")
        assert s.vm.wait(timeout=20) == 0

    # Verify every distinct name in the newest committed snapshot.
    with disk.open("rb") as f:
        heads = []
        for base in SLOTS:
            f.seek(base)
            h = f.read(512)
            if h.startswith(b"LIXINFS3"):
                heads.append((struct.unpack_from("<Q", h, 8)[0], base))
        _, base = max(heads)
        f.seek(base + 512)
        entries, = struct.unpack("<I", f.read(4))
        found = 0
        for _ in range(entries):
            n, size, _, _ = struct.unpack("<IIII", f.read(16))
            name = f.read(n)
            if size != 0xffffffff:
                if name.startswith(b"/batch"):
                    assert name == f"/batch{found // 100000}/{found % 100000:06d}".encode()
                    found += 1
                f.seek(size, 1)
        assert found == 1_000_000, found

    with boot(disk) as s:
        command(s, "cat /batch9/099999", b"last\n")
        assert re.search(rb"0  [^\n]+ UTC  /batch0/000000\n", command(s, "ls -l /batch0/000000"))
        command(s, "cat big | wc -l", b"524288\n")
        assert re.search(rb"1048576  [^\n]+ UTC  big\n", command(s, "ls -l big"))


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as directory:
        test(Path(directory) / "scale.img")
    print("Verified 1,000,000 distinct guest-created files and a 1 MiB file after reboot.")
