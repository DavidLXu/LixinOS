"""Exercise the native guest client and verify the export with Apple's Git."""
from pathlib import Path
import re
import runpy
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
boot = runpy.run_path(str(ROOT / "scripts/test-user.py"))["boot"]


def command(serial, line, *expected, error=False):
    serial.send(line.encode() + b"\r")
    body = serial.until(b"$ ").split(b"\n", 1)[1]
    if not error:
        assert b"shell: " not in body and b"git: " not in body, (line, body)
    for value in expected:
        assert value in body, (line, body)
    return body


def test(temp):
    disk = temp / "test.img"
    with boot(disk) as serial:
        for line in ("mkdir project", "cd project", "git init", "mkdir src",
                     "echo 'int main(void){return 0;}' > src/main.c",
                     "tcc src/main.c -o src/prog", "echo alpha > 'a file'"):
            command(serial, line)
        command(serial, "git status", b"?? a file", b"?? src/main.c", b"?? src/prog")
        command(serial, "git add .")
        command(serial, "git diff --cached", b"+alpha", b"Binary files")
        command(serial, "git commit -m 'first version'", b"first version")
        command(serial, "git status", b"Working tree clean")
        command(serial, "echo bravo > 'a file'")
        command(serial, "git diff", b"-alpha", b"+bravo")
        command(serial, "git add 'a file' missing", b"path does not exist", error=True)
        command(serial, "git status", b" M a file")
        command(serial, "git add 'a file'")
        command(serial, "git commit -m second")
    with boot(disk) as serial:
        command(serial, "cd project")
        log = command(serial, "git log", b"first version", b"second")
        ids = re.findall(rb"^commit ([0-9a-f]{40})$", log, re.M)
        assert len(ids) == 2
        command(serial, "cd src")
        command(serial, "rm main.c")
        command(serial, "git add main.c")
        command(serial, "cd ..")
        command(serial, "echo delta > 'a file'")
        command(serial, "git add 'a file'")
        command(serial, "echo gamma > 'a file'")
        command(serial, "echo local > notes")
        command(serial, "git status", b"MM a file", b"D  src/main.c", b"?? notes")
        command(serial, "git diff --staged", b"-bravo", b"+delta")
        command(serial, "git diff", b"-delta", b"+gamma")

    destination = temp / "export"
    subprocess.run([ROOT / "scripts/export-git.py", "--disk", disk, "/project", destination], check=True)

    def git(*args):
        return subprocess.check_output(["/usr/bin/git", "-C", destination, *args])

    git("fsck", "--full", "--strict")
    assert git("rev-list", "HEAD").splitlines() == ids
    assert git("show", "HEAD:a file") == b"bravo\n"
    assert git("show", ":a file") == b"delta\n"
    assert (destination / "a file").read_bytes() == b"gamma\n"
    assert git("show", "HEAD:src/prog") == (destination / "src/prog").read_bytes()
    assert git("status", "--porcelain", "-z") == b"MM a file\0D  src/main.c\0?? notes\0"
    assert b"+gamma" in git("diff") and b"+delta" in git("diff", "--cached")
    git("-c", "user.name=Mac", "-c", "user.email=mac@example.org", "commit", "-qm", "continued on Mac")
    assert git("rev-parse", "HEAD^").strip() == ids[0]
    git("fsck", "--full", "--strict")

    # Fill the disposable disk's latest snapshot to its byte limit so that
    # creating one more blob must roll back the staged index.
    codec = runpy.run_path(str(ROOT / "scripts/export-git.py"))
    from disk_image import SLOT
    slot = SLOT
    with disk.open("r+b") as f:
        heads = []
        for base in (512, 512 + slot):
            f.seek(base)
            header = f.read(512)
            if header.startswith(b"LIXINFS3"):
                heads.append((struct.unpack_from("<Q", header, 8)[0], base, header))
        gen, base, header = max(heads)
        f.seek(base + 512)
        data = bytearray(f.read(struct.unpack_from("<I", header, 16)[0]))
        count, = struct.unpack_from("<I", data)
        while len(data) < slot - 512:
            name = f"/pad{count}".encode()
            size = min(1 << 20, slot - 512 - len(data) - len(name) - 16)
            assert size >= 0
            data.extend(struct.pack("<IIII", len(name), size, 0, 0) + name + bytes(size))
            count += 1
        struct.pack_into("<I", data, 0, count)
        h = struct.pack("<8sQII", b"LIXINFS3", gen + 1, len(data), codec["checksum"](data))
        f.seek(512 + ((gen + 1) % 2) * slot)
        f.write((h + struct.pack("<I", codec["checksum"](h))).ljust(512, b"\0"))
        f.write(data)
    with boot(disk) as serial:
        command(serial, "cd project")
        command(serial, "git add 'a file'", b"filesystem full", error=True)
    with boot(disk) as serial:
        command(serial, "cd project")
        command(serial, "git diff --staged", b"+delta")
        command(serial, "git diff", b"-delta", b"+gamma")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as directory:
        test(Path(directory))
    print("Guest Git, reboot, export and Mac Git checks passed.")
