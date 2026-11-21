"""Stage and reboot a one-MiB C source through the guest's libgit2 port."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import subprocess

from disk_image import edit_files

ROOT = Path(__file__).resolve().parents[1]
boot = runpy.run_path(str(ROOT / "scripts/test-user.py"))["boot"]


def source():
    # 16,384 distinct, valid C comment lines; compressed Git objects also span packets.
    return b"".join(b"/*"+hashlib.sha256(i.to_bytes(4,"big")).hexdigest()[:59].encode()+b"*/\n"
                    for i in range(16384))


def check(s, line, expected=None):
    s.send(line.encode()+b"\r")
    out = s.until(b"$ ", timeout=90).split(b"\n", 1)[-1]
    assert b"git: " not in out and b"shell: " not in out, (line, out)
    if expected:
        assert expected in out, (line, out[:1000])
    return out


def test(disk, cmd):
    data = source()
    assert len(data) == 1 << 20
    def setup(rows):
        if b"/large-git" in rows:
            raise ValueError("use a fresh disposable disk")
        rows[b"/large-git"] = bytes(8), None
        rows[b"/large-git/big.c"] = bytes(8), data
    edit_files(disk, setup)
    with boot(disk, cmd, prompt=b"$ ") as s:
        check(s,"cd /large-git")
        check(s,"git init", b"Git repository ready")
        check(s,"git add big.c")
        check(s,"git commit -m large", b"large")
        check(s,"git status",b"Working tree clean")
        s.send(b"shutdown\r")
        assert s.vm.wait(timeout=15) == 0
    with boot(disk, cmd, prompt=b"$ ") as s:
        check(s,"cd /large-git")
        check(s,"git log",b"large")
        check(s,"echo '// reboot diff' >> big.c")
        check(s,"git diff",b"+// reboot diff")
    from tempfile import TemporaryDirectory
    with TemporaryDirectory() as tmp:
        dest = Path(tmp) / "repo"
        subprocess.run([ROOT/"scripts/export-git.py","--disk",disk,"/large-git",dest], check=True)
        subprocess.run(["git","-C",dest,"fsck","--strict"],check=True)
        assert subprocess.check_output(["git","-C",dest,"show","HEAD:big.c"]) == data
    print("Verified 1 MiB source, reboot diff, Mac Git blob contents and fsck.", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("disk",type=Path)
    parser.add_argument("--kernel",choices=["rust","tcc"],default="rust")
    parser.add_argument("--release")
    args = parser.parse_args()
    disk = args.disk.resolve()
    os.environ["LIXINOS_TCC_DISK"] = str(disk)
    cmd = [ROOT/"scripts/run-kernel.sh","--serial","--kernel",args.kernel]
    cmd += ["--release",args.release] if args.release else ["--source"]
    test(disk, cmd)
