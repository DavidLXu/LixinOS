"""Check PDF thumbnails, remembered position, zoom, scroll, paging and resize on both kernels."""
import argparse
import os
from pathlib import Path
import runpy
import tempfile
from unittest.mock import patch

from disk_image import disk_entries

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "Kernel/target/user-build/pdfview/package"
BOOT = runpy.run_path(str(ROOT / "scripts/test-panes.py"))["test"]
WRITE = runpy.run_path(str(ROOT / "User/rust/bootstrap-disk.py"))["write_volume"]


def sample(path):
    from reportlab.pdfgen import canvas

    document = canvas.Canvas(str(path), pagesize=(420, 540))
    for number, color in enumerate(("#195c8c", "#b23d1f"), 1):
        document.setFillColor(color)
        document.rect(30, 400, 340, 80, fill=1, stroke=0)
        document.setFillColor("white")
        document.drawString(50, 440, f"Page {number}")
        document.showPage()
    document.save()


def exercise(guest, control, directory):
    for page in (1, 2):
        guest.send(f"pdfview.win3 --render /reading.pdf {page} /p{page}.png\r".encode())
        output = guest.until(b"$ ", timeout=120)
        assert f"{page}/2 420x540".encode() in output and b"[exit" not in output, output
    guest.send(b"tcc /test.c -o /bin/pdf-test\r")
    output = guest.until(b"$ ", timeout=60)
    assert b"error:" not in output, output
    guest.send(b"pdf-test\r")
    output = guest.until(b"$ ", timeout=120)
    assert b"PDF window passed" in output, output
    guest.send(b"q\r")
    assert guest.vm.wait(timeout=80) == 0


def run(seed, kernels, release):
    rows, _ = disk_entries(seed)
    required = {"/lib/lxe-runtime.elf", "/bin/tcc", "/bin/bash", "/bin/sh",
                "/bin/shutdown", "/src/console.h"}
    prefixes = ("/boot/", "/lib/gtk3/", "/include/", "/src/tcc/", "/src/tinycc/",
                "/lib/tcc/")
    rows = {path: record for path, record in rows.items()
            if path in required or path.startswith(prefixes) or
            (record[1] is None and path.count("/") <= 3)}
    for filename in ("gui/text-clipboard.h", "desktop/window-client.h", "desktop/window-client-host.h"):
        rows["/src/" + filename] = bytes(8), (ROOT / "User" / filename).read_bytes()
    rows["/test.c"] = bytes(8), (ROOT / "User/pdfview/test-host.c").read_bytes()
    for path in BUILD.rglob("*"):
        rows["/" + path.relative_to(BUILD).as_posix()] = bytes(8), path if path.is_file() else None
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        sample(tmp / "reading.pdf")
        rows["/reading.pdf"] = bytes(8), tmp / "reading.pdf"
        for kernel in kernels:
            disk = tmp / (kernel + ".img")
            WRITE(rows, b"", disk, with_loader=True)
            with patch.dict(os.environ, {"LIXINOS_TCC_DISK": str(disk)}):
                BOOT(exercise, mode="--window", disk=disk, prompt=b"$ ",
                     launcher_args=("--release", release, "--kernel", kernel))
            saved, _ = disk_entries(disk)
            for page in (1, 2):
                assert bytes(saved[f"/p{page}.png"][1][:8]) == b"\x89PNG\r\n\x1a\n"
            print(f"{kernel}: PDF raster output, thumbnails, remembered position, zoom, fit, "
                  "wheel scrolling, page navigation and resize passed", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=Path, default=ROOT / "data/shared.img")
    parser.add_argument("--kernel", choices=("rust", "tcc", "both"), default="both")
    parser.add_argument("--release", default="latest")
    args = parser.parse_args()
    run(args.seed, ("rust", "tcc") if args.kernel == "both" else (args.kernel,), args.release)
