"""Check the VirtIO-only scanout, resizing and packed canvas tiles on real QEMU."""
import argparse
import os
from pathlib import Path
import runpy
import tempfile
import time
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
MUTTER = runpy.run_path(str(HERE / "test-mutter.py"))
HARNESS = runpy.run_path(str(HERE / "test-panes.py"))
RESIZE = runpy.run_path(str(HERE / "test-resize.py"))["resize_source"]
PROGRAM = (
    "#include <stdio.h>",
    "#include <lixin.h>",
    "unsigned long lx_canvas_size(void);",
    "int lx_canvas_present(const unsigned *,unsigned,unsigned,unsigned,unsigned);",
    "int main(void){unsigned *p=(unsigned*)0x68000000; if(!lx_canvas_size())return 1;",
    "for(int i=0;i<64*32;i++)p[i]=0x318ab4;",
    "if(lx_canvas_present(p,41,53,64,32))return 2;",
    'puts("first tile");getchar();',
    "for(int i=0;i<13*7;i++)p[i]=(i%13<5)?0xe49835:0x57b661;",
    "if(lx_canvas_present(p,54,62,13,7))return 3;",
    'puts("second tile");getchar();return 0;}',
)


def exercise(guest, control, temp):
    qmp = MUTTER["qmp_connection"](control)
    qmp("qmp_capabilities")
    frame = temp / "gpu.ppm"
    source = Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"])

    def wait_frame(size, tile=None):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            qmp("screendump", filename=str(frame))
            _, dims, _, pixels = frame.read_bytes().split(b"\n", 3)
            if dims == f"{size[0]} {size[1]}".encode():
                if tile is None:
                    if bytes.fromhex("d4e3ff") in pixels[:44 * (len(pixels) // size[1])]:
                        return
                else:
                    pitch = len(pixels) // size[1]
                    if all(pixels.startswith(row, (44 + 106 + y) * pitch + 82 * 3)
                           for y, row in enumerate(tile)):
                        return
            time.sleep(.05)
        raise AssertionError(f"VirtIO scanout failed pixel check at {size}, last dimensions {dims!r}")

    def resize(size):
        source.write_text(f"{size[0]}\t{size[1]}\n")
        wait_frame(size)

    for i, line in enumerate(PROGRAM):
        redirect = ">" if i == 0 else ">>"
        guest.expect(f"echo '{line}' {redirect} gpu-test.c\r".encode(), b"$ ")
    guest.expect(b"tcc gpu-test.c -o gpu-test\r", b"$ ")
    size = (1600, 1056)
    resize(size)
    base, orange, green = (bytes.fromhex(c) * 2 for c in ("318ab4", "e49835", "57b661"))
    initial = [base * 64] * 64
    guest.send(b"./gpu-test\r")
    guest.until(b"first tile", timeout=10)
    wait_frame(size, initial)
    guest.send(b"\r")
    guest.until(b"second tile", timeout=10)
    updated = initial.copy()
    updated[18:32] = [base * 13 + orange * 5 + green * 8 + base * 38] * 14
    wait_frame(size, updated)
    guest.send(b"\r")
    guest.until(b"$ ")
    resize((1320, 880))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", choices=("rust", "tcc"), action="append")
    parser.add_argument("--release", default="latest")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="lixin-gpu-") as temp:
        disk = Path(temp) / "gpu.img"
        # The Rust boot seeds the font and compiler used by the TCC kernel.
        with MUTTER["DBUS"]["boot"](disk, args.release, "rust") as guest:
            MUTTER["GUEST"]["check_boot_release"](guest, b"rust")
            MUTTER["GUEST"]["shutdown"](guest)
        for kernel in args.kernel or ("rust", "tcc"):
            with patch.dict(os.environ, {"LIXINOS_GPU": "virtio", "LIXINOS_TCC_DISK": str(disk)}), RESIZE():
                HARNESS["test"](exercise, mode="--window", disk=disk, prompt=b"$ ",
                                launcher_args=("--kernel", kernel, "--release", args.release))
            print(f"{kernel}: VirtIO GPU scanout, two modes and exact tile pixels passed", flush=True)
