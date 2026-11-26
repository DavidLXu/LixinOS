"""Measure QEMU tablet input to the VirGL scanout backing on a disposable Desktop disk."""
import argparse
import os
from pathlib import Path
import runpy
import statistics
import subprocess
import tempfile
import time
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
GTK = runpy.run_path(str(HERE / "test-gtk3.py"))
GNOME = runpy.run_path(str(HERE / "test-gnome-shell.py"))
WIDTH, HEIGHT = GTK["WIDTH"], GTK["HEIGHT"]
Y = 160
# Win3's 2x canvas: the first line of the cursor starts with one outline pixel.
HOTSPOT = bytes.fromhex("32251b00") * 2


def measure(guest, control, directory):
    qmp = GNOME["MUTTER"]["qmp_connection"](control)
    qmp("qmp_capabilities")
    qmp("trace-event-set-state", name="address_space_map", enable=True)
    Path(os.environ["LIXINOS_TEST_WINDOW_SIZE"]).write_text(f"{WIDTH}\t{HEIGHT}\n")
    guest.until(b"$ ")
    deadline = time.monotonic() + 12
    while True:
        try:
            address, stride = GNOME["virgl_scanout"](directory, WIDTH, HEIGHT)
            break
        except AssertionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.05)
    qmp("trace-event-set-state", name="address_space_map", enable=False)

    row_file = directory / "scanline"
    def row(y):
        qmp("pmemsave", val=address + (44 + 2*y)*stride*4, size=WIDTH*4, filename=str(row_file))
        return row_file.read_bytes()

    def pointer_x():
        offset = row(Y).find(HOTSPOT, 170*8)
        return offset // 8 if offset >= 0 else None

    def move(x):
        axes = (("x", 2*x+1, WIDTH), ("y", 44+2*Y+1, HEIGHT))
        qmp("input-send-event", events=[
            {"type": "abs", "data": {"axis": axis, "value": round(pixel*32767/(extent-1))}}
            for axis, pixel, extent in axes])

    guest.send(b"desktop\r")
    deadline = time.monotonic() + 20
    while row(2)[16:19] != bytes.fromhex("2b1e28"):
        if time.monotonic() >= deadline:
            raise AssertionError("Desktop dock did not appear")
        time.sleep(.04)

    latency = []
    for index in range(51):
        target = 211 + (index*47) % 500
        start = time.monotonic()
        move(target)
        while pointer_x() != target:
            if time.monotonic() - start > 2:
                raise AssertionError(f"Cursor did not reach {target}")
            time.sleep(.0007)
        if index:
            latency.append(1000*(time.monotonic() - start))
        time.sleep(.007 + (index % 7)*.002)

    flush_event = "virtio_gpu_cmd_res_flush"
    qmp("trace-event-set-state", name=flush_event, enable=True)
    start = time.monotonic()
    next_step = 0
    last_x = None
    positions = set()
    while time.monotonic() - start < 3:
        step = int((time.monotonic() - start)*120)
        if step >= next_step and step < 360:
            move(201 + step)
            next_step = step + 1
        x = pointer_x()
        if x is not None and 201 <= x < 201 + next_step:
            positions.add(x)
            last_x = x
        time.sleep(.0008)
    assert last_x is not None and last_x > 530, "Cursor stalled during sweep"
    duration = time.monotonic() - start
    qmp("trace-event-set-state", name=flush_event, enable=False)
    flushes = (directory / "qmp.trace").read_text().count(flush_event + " ")
    assert flushes, "QEMU did not record a scanout flush"
    print(f"QEMU scanout flushes/s {flushes/duration:.1f}, "
          f"distinct pointer positions/s {len(positions)/duration:.1f}, "
          f"input-to-scanout latency median {statistics.median(latency):.1f} ms, "
          f"p95 {sorted(latency)[47]:.1f} ms", flush=True)
    guest.send(b"\x1b")
    guest.until(b"$ ")
    GTK["SHUTDOWN"](guest)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", type=Path, default=HERE.parent / "data/desktop-pingfang.img")
    parser.add_argument("--release", default="latest")
    parser.add_argument("--kernel", choices=("rust", "tcc"), default="rust")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp, GTK["RESIZE"]():
        disk = Path(temp) / "desktop.img"
        subprocess.run(["cp", "-c", args.disk, disk], check=True)
        with patch.dict(os.environ, {"LIXINOS_GPU": "virgl", "LIXINOS_TCC_DISK": str(disk)}):
            GTK["HARNESS"]["test"](measure, mode="--window", disk=disk, prompt=b"LixinOS Bash (C shell)",
                                   launcher_args=("--kernel", args.kernel, "--release", args.release))
