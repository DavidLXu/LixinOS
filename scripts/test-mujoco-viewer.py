"""Exercise the upstream simulate UI, renderer and model loader over native Win3."""
import argparse
import os
from pathlib import Path
import runpy
import struct
import subprocess
import tempfile
from unittest.mock import patch

from disk_image import disk_entries, edit_files

ROOT = Path(__file__).resolve().parents[1]
HARNESS = runpy.run_path(str(ROOT / "scripts/test-panes.py"))
INSTALL = runpy.run_path(str(ROOT / "User/gcc/install.py"))["package_edit"]


def exercise(guest, control, directory):
    guest.until(b"$ ")
    guest.send(b"tcc /viewer-test.c -o /bin/viewer-test\r")
    output = guest.until(b"$ ", timeout=30)
    assert b"error:" not in output, output
    guest.send(b"viewer-test\r")
    output = guest.until(b"$ ", timeout=240)
    assert b"MuJoCo viewer passed" in output, output
    print(output.decode(errors="replace"), flush=True)
    guest.expect(b"echo $?\r", b"$ ", b"\n0\n")
    guest.send(b"shutdown\r")
    assert guest.vm.wait(timeout=15) == 0


def run(kernel, release):
    gpu = "virgl" if kernel == "rust" else "ramfb"
    with tempfile.TemporaryDirectory() as tmp:
        disk = Path(tmp) / "simulate.img"
        subprocess.run(["cp", "-c", ROOT / "data/desktop-pingfang.img", disk], check=True)

        def stage(rows):
            INSTALL(ROOT / "Kernel/target/user-build/simulate-package", "simulate")(rows)
            for directory in ("gui", "desktop"):
                rows.setdefault(f"/src/{directory}".encode(), (bytes(8), None))
            for source in ("gui/text-clipboard.h", "desktop/window-client.h", "desktop/window-client-host.h"):
                rows[f"/src/{source}".encode()] = bytes(8), (ROOT / "User" / source).read_bytes()
            rows[b"/viewer-test.c"] = bytes(8), (ROOT / "User/mujoco/viewer/test-host.c").read_bytes()

        edit_files(disk, stage)
        with patch.dict(os.environ, {"LIXINOS_TCC_DISK": str(disk), "LIXINOS_GPU": gpu}):
            HARNESS["test"](exercise, mode="--window", disk=disk, prompt=b"LixinOS Bash (C shell)",
                            launcher_args=("--release", release, "--kernel", kernel))
        rows, _ = disk_entries(disk)
        png = bytes(rows["/screenshot.png"][1])
        assert png[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", png[16:24]) == (721, 437)
        assert b"Humanoid" in rows["/mjmodel.xml"][1].tobytes()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", choices=("rust", "tcc", "both"), default="both")
    parser.add_argument("--release", default="latest")
    args = parser.parse_args()
    for kind in ("rust", "tcc") if args.kernel == "both" else (args.kernel,):
        run(kind, args.release)
