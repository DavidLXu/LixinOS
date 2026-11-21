"""Build the desktop game with LixinOS's own TCC on a temporary disk copy."""
import argparse
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from disk_image import disk_entries, edit_files

def build(source, release):
    serial = runpy.run_path(str(ROOT / "scripts/test-user.py"))
    package = ROOT / "Kernel/target/user-build/2048-package"
    library = package / "lib/2048"
    with tempfile.TemporaryDirectory(prefix="lixin-2048-") as temporary:
        disk = Path(temporary) / "build.img"
        subprocess.run(["cp", "-c", source, disk], check=True)

        def stage(rows):
            rows[b"/lib/2048"] = (bytes(8), None)
            for name in ("board.h", "game.c", "LICENSE"):
                rows[f"/lib/2048/{name}".encode()] = (bytes(8), (HERE/name).read_bytes())

        edit_files(disk, stage)
        launch = [ROOT/"scripts/run-kernel.sh", "--serial", "--kernel", "rust", "--release", release]
        with serial["boot"](disk, launch, prompt=b"$ ") as guest:
            guest.expect(b"cd /\r", serial["SHELL"])
            guest.send(b"tcc /lib/2048/game.c -o /bin/2048.win3\r")
            log = guest.until(serial["SHELL"], timeout=90)
            guest.send(b"echo $?\r")
            if b"\n0\n" not in guest.until(serial["SHELL"]):
                raise RuntimeError(log.decode(errors="replace"))
            guest.send(b"q\r")
            if guest.vm.wait(timeout=20):
                raise RuntimeError("Build VM failed to shut down")
        rows, _ = disk_entries(disk)
        executable = bytes(rows["/bin/2048.win3"][1])
        if not executable.startswith(b"LXE1"):
            raise ValueError("Expected a guest-built LXE1 executable")
        library.mkdir(parents=True, exist_ok=True)
        for name in ("game.c", "board.h", "LICENSE"):
            shutil.copyfile(HERE/name, library/name)
        (package/"bin").mkdir(exist_ok=True)
        (package/"bin/2048.win3").write_bytes(executable)
    print(package)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", type=Path, help="desktop disk with TCC and window-app.h")
    parser.add_argument("--release", default="latest")
    options = parser.parse_args()
    build(options.disk.resolve(strict=True), options.release)
